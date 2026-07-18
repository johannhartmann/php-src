#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_ssa.h"

typedef struct _frontend_fixture {
	zend_op_array op_array;
	zend_op opcodes[3];
	zval literals[5];
	zend_ssa ssa;
	zend_ssa_op ssa_ops[3];
	zend_ssa_var ssa_vars[3];
	zend_ssa_var_info var_info[3];
	zend_ssa_block ssa_blocks[1];
	zend_basic_block cfg_blocks[1];
	uint32_t cfg_map[3];
} frontend_fixture;

static void init_ssa_op(zend_ssa_op *op)
{
	memset(op, 0xff, sizeof(*op));
}

static void init_fixture(frontend_fixture *fixture)
{
	memset(fixture, 0, sizeof(*fixture));
	init_ssa_op(&fixture->ssa_ops[0]);
	init_ssa_op(&fixture->ssa_ops[1]);
	init_ssa_op(&fixture->ssa_ops[2]);

	fixture->op_array.last_var = 1;
	fixture->op_array.T = 1;
	fixture->op_array.last = 2;
	fixture->op_array.opcodes = fixture->opcodes;
	fixture->op_array.last_literal = 0;
	fixture->op_array.literals = NULL;

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
	fixture->ssa.var_info[1].type = MAY_BE_LONG;
	fixture->ssa.var_info[1].has_range = true;
	fixture->ssa.var_info[1].range.min = 1;
	fixture->ssa.var_info[1].range.max = 10;

	fixture->ssa.cfg.blocks_count = 1;
	fixture->ssa.cfg.edges_count = 0;
	fixture->ssa.cfg.blocks = fixture->cfg_blocks;
	fixture->ssa.cfg.map = fixture->cfg_map;
	fixture->cfg_blocks[0].flags = ZEND_BB_REACHABLE;
	fixture->cfg_blocks[0].start = 0;
	fixture->cfg_blocks[0].len = 2;
	fixture->cfg_blocks[0].predecessor_offset = -1;
}

static zend_mir_lowering_status initialize(
	frontend_fixture *fixture,
	zend_mir_zend_source *source,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_init(
		source, &fixture->op_array, &fixture->ssa, 17, 23, diagnostic);
}

static void test_source_view_and_ids(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_opcode_ref opcode;
	zend_mir_source_ssa_ref ssa_ref;
	zend_mir_source_ssa_use_ref use;
	zend_mir_source_ssa_def_ref def;
	zend_mir_source_slot_ref slot;
	zend_mir_source_position_ref position;
	zend_mir_value_fact_ref fact;

	init_fixture(&fixture);
	assert(initialize(&fixture, &source, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(diagnostic.code == ZEND_MIRL_OK);
	assert(zend_mir_zend_source_view(&source, &view));
	assert(view.contract_version == ZEND_MIR_CONTRACT_VERSION);
	assert(view.opcode_count(view.context) == 2);
	assert(view.ssa_count(view.context) == 2);
	assert(view.ssa_use_count(view.context) == 2);
	assert(view.ssa_def_count(view.context) == 1);
	assert(view.literal_count(view.context) == 0);

	assert(view.opcode_at(view.context, 0, &opcode));
	assert(opcode.opline_index == 0);
	assert(opcode.zend_opcode_number == ZEND_QM_ASSIGN);
	assert(opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_SSA);
	assert(opcode.op1.ssa_variable_id == 0);
	assert(opcode.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV);
	assert(opcode.op1.index == 0);
	assert(opcode.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA);
	assert(opcode.result.ssa_variable_id == 1);
	assert(opcode.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP);
	assert(opcode.result.index == 0);
	assert(!view.opcode_at(view.context, 2, &opcode));
	assert(!view.opcode_at(view.context, 0, NULL));

	assert(view.ssa_at(view.context, 0, &ssa_ref));
	assert(ssa_ref.ssa_variable_id == 0);
	assert(ssa_ref.definition_opline_index == ZEND_MIR_ID_INVALID);
	assert(view.ssa_at(view.context, 1, &ssa_ref));
	assert(ssa_ref.ssa_variable_id == 1);
	assert(ssa_ref.definition_opline_index == 0);
	assert(ssa_ref.source_slot_kind == ZEND_MIR_SOURCE_SLOT_TMP);

	assert(view.ssa_use_at(view.context, 0, &use));
	assert(use.ssa_variable_id == 0 && use.opline_index == 0
		&& use.operand_index == 0);
	assert(view.ssa_use_at(view.context, 1, &use));
	assert(use.ssa_variable_id == 1 && use.opline_index == 1);
	assert(view.ssa_def_at(view.context, 0, &def));
	assert(def.ssa_variable_id == 1 && def.opline_index == 0);
	assert(def.destination.ssa_variable_id == 1);

	assert(zend_mir_zend_source_slot_count(&source) == 3);
	assert(zend_mir_zend_source_slot_at(&source, 0, &slot));
	assert(slot.kind == ZEND_MIR_SOURCE_SLOT_CV && slot.kind_index == 0);
	assert(zend_mir_zend_source_slot_at(&source, 1, &slot));
	assert(slot.kind == ZEND_MIR_SOURCE_SLOT_TMP && slot.kind_index == 0);
	assert(zend_mir_zend_source_slot_at(&source, 2, &slot));
	assert(slot.kind == ZEND_MIR_SOURCE_SLOT_VAR && slot.kind_index == 0);
	assert(!zend_mir_zend_source_slot_at(&source, 3, &slot));

	assert(zend_mir_zend_source_position_count(&source) == 2);
	assert(zend_mir_zend_source_position_at(&source, 1, &position));
	assert(position.id == 1 && position.file_symbol_id == 23);
	assert(position.line == 11 && position.column_start == 0
		&& position.column_end == 0);

	assert(zend_mir_zend_source_value_fact_count(&source) == 2);
	assert(zend_mir_zend_source_value_fact_at(&source, 0, &fact));
	assert(fact.id == 0 && fact.value_id == 0);
	assert(fact.exact_type == ZEND_MIR_SCALAR_TYPE_I64);
	assert(fact.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE);
	assert(fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED);
	assert(fact.flags & ZEND_MIR_VALUE_FACT_NONZERO);
	assert(fact.integer_min == 1 && fact.integer_max == 10);
	assert(zend_mir_zend_source_value_fact_at(&source, 1, &fact));
	assert(fact.id == 1 && fact.value_id == 1);
	assert(!zend_mir_zend_source_value_fact_at(&source, 2, &fact));
}

static void test_exact_scalar_fact_mapping(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_value_fact_ref fact;

	init_fixture(&fixture);
	fixture.op_array.last_var = 3;
	fixture.op_array.T = 0;
	fixture.op_array.last = 1;
	memset(&fixture.opcodes[0], 0, sizeof(fixture.opcodes[0]));
	init_ssa_op(&fixture.ssa_ops[0]);
	fixture.opcodes[0].opcode = ZEND_NOP;
	fixture.cfg_blocks[0].len = 1;
	fixture.ssa.vars_count = 3;
	fixture.ssa.vars[0].var = 0;
	fixture.ssa.vars[0].definition = -1;
	fixture.ssa.vars[1].var = 1;
	fixture.ssa.vars[1].definition = -1;
	fixture.ssa.vars[2].var = 2;
	fixture.ssa.vars[2].definition = -1;
	fixture.ssa.var_info[0].type = MAY_BE_NULL;
	fixture.ssa.var_info[0].has_range = false;
	fixture.ssa.var_info[1].type = MAY_BE_BOOL;
	fixture.ssa.var_info[1].has_range = false;
	fixture.ssa.var_info[2].type = MAY_BE_DOUBLE;
	fixture.ssa.var_info[2].has_range = false;

	assert(initialize(&fixture, &source, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_value_fact_count(&source) == 3);
	assert(zend_mir_zend_source_value_fact_at(&source, 0, &fact));
	assert(fact.value_id == 0 && fact.exact_type == ZEND_MIR_SCALAR_TYPE_NULL);
	assert(zend_mir_zend_source_value_fact_at(&source, 1, &fact));
	assert(fact.value_id == 1 && fact.exact_type == ZEND_MIR_SCALAR_TYPE_I1);
	assert(zend_mir_zend_source_value_fact_at(&source, 2, &fact));
	assert(fact.value_id == 2 && fact.exact_type == ZEND_MIR_SCALAR_TYPE_F64);
	assert((fact.flags & ZEND_MIR_VALUE_FACT_FINITE) == 0);
}

static void init_literal_fixture(frontend_fixture *fixture)
{
	init_fixture(fixture);
	fixture->op_array.last_var = 0;
	fixture->op_array.T = 1;
	fixture->op_array.last_literal = 5;
	fixture->op_array.literals = fixture->literals;
	ZVAL_NULL(&fixture->literals[0]);
	ZVAL_FALSE(&fixture->literals[1]);
	ZVAL_TRUE(&fixture->literals[2]);
	ZVAL_LONG(&fixture->literals[3], -42);
	ZVAL_DOUBLE(&fixture->literals[4], -1.5);
	fixture->opcodes[0].op1_type = IS_CONST;
	fixture->opcodes[0].op1.constant = 3;
	fixture->opcodes[0].result.var = EX_NUM_TO_VAR(0);
	fixture->opcodes[1].op1.var = EX_NUM_TO_VAR(0);
	fixture->ssa_ops[0].op1_use = -1;
	fixture->ssa_ops[0].result_def = 0;
	fixture->ssa_ops[1].op1_use = 0;
	fixture->ssa.vars_count = 1;
	fixture->ssa.vars[0].var = 0;
	fixture->ssa.vars[0].definition = 0;
	fixture->ssa.var_info[0].type = MAY_BE_LONG;
}

static void test_literal_canonicalization(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_literal_ref literal;
	uint64_t expected_double;
	double value = -1.5;

	init_literal_fixture(&fixture);
	assert(initialize(&fixture, &source, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_view(&source, &view));
	assert(view.literal_count(view.context) == 5);
	assert(view.literal_at(view.context, 0, &literal));
	assert(literal.kind == ZEND_MIR_SOURCE_LITERAL_NULL
		&& literal.payload_bits == 0);
	assert(view.literal_at(view.context, 1, &literal));
	assert(literal.kind == ZEND_MIR_SOURCE_LITERAL_FALSE);
	assert(view.literal_at(view.context, 2, &literal));
	assert(literal.kind == ZEND_MIR_SOURCE_LITERAL_TRUE);
	assert(view.literal_at(view.context, 3, &literal));
	assert(literal.kind == ZEND_MIR_SOURCE_LITERAL_LONG_BITS);
	assert(literal.payload_bits == (uint64_t) -42);
	memcpy(&expected_double, &value, sizeof(expected_double));
	assert(view.literal_at(view.context, 4, &literal));
	assert(literal.kind == ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS);
	assert(literal.payload_bits == expected_double);
	assert(!view.literal_at(view.context, 5, &literal));
}

static void test_pass_two_literal_address(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_opcode_ref opcode;
#if !ZEND_USE_ABS_CONST_ADDR
	int64_t offset;
#endif

	init_literal_fixture(&fixture);
#if ZEND_USE_ABS_CONST_ADDR
	fixture.opcodes[0].op1.zv = &fixture.literals[3];
#else
	offset = (int64_t) (uintptr_t) &fixture.literals[3]
		- (int64_t) (uintptr_t) &fixture.opcodes[0];
	assert(offset >= INT32_MIN && offset <= INT32_MAX);
	fixture.opcodes[0].op1.constant = (uint32_t) (int32_t) offset;
#endif
	fixture.op_array.fn_flags |= ZEND_ACC_DONE_PASS_TWO;
	assert(initialize(&fixture, &source, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_view(&source, &view));
	assert(view.opcode_at(view.context, 0, &opcode));
	assert(opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL);
	assert(opcode.op1.index == 3);
}

static void assert_failure_atomic(
	frontend_fixture *fixture,
	zend_mir_lowering_status expected_status,
	zend_mir_lowering_diagnostic_code expected_code)
{
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;

	memset(&source, 0xa5, sizeof(source));
	assert(initialize(fixture, &source, &diagnostic) == expected_status);
	assert(diagnostic.status == expected_status);
	assert(diagnostic.code == expected_code);
	assert(!zend_mir_zend_source_view(&source, &view));
	assert(source.initialized == 0);
	assert(source.op_array == NULL && source.ssa == NULL);
}

static void test_malformed_sources(void)
{
	frontend_fixture fixture;

	init_fixture(&fixture);
	fixture.opcodes[0].op1_type = 0xff;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_fixture(&fixture);
	fixture.opcodes[0].op1.var = 0;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_fixture(&fixture);
	fixture.ssa_ops[0].op1_use = 2;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_fixture(&fixture);
	fixture.ssa_ops[0].result_def = -1;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_literal_fixture(&fixture);
	fixture.opcodes[0].op1.constant = 5;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_fixture(&fixture);
	fixture.ssa.cfg.map = NULL;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);

	init_fixture(&fixture);
	fixture.ssa.cfg.blocks_count = 2;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);
}

static void test_invalid_stable_ids(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;

	init_fixture(&fixture);
	assert(zend_mir_zend_source_init(
			&source, &fixture.op_array, &fixture.ssa, ZEND_MIR_ID_INVALID, 23,
			&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic.code == ZEND_MIRL_INVALID_SOURCE);
	assert(source.initialized == 0);
	assert(zend_mir_zend_source_init(
			&source, &fixture.op_array, &fixture.ssa, 17, ZEND_MIR_ID_INVALID,
			&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic.code == ZEND_MIRL_INVALID_SOURCE);
	assert(source.initialized == 0);
}

static void test_conservative_facts(void)
{
	frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_value_fact_ref fact;

	init_fixture(&fixture);
	fixture.ssa.var_info[0].type = MAY_BE_LONG | MAY_BE_DOUBLE;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED, ZEND_MIRL_MISSING_PROOF);

	init_fixture(&fixture);
	fixture.ssa.vars[0].alias = SYMTABLE_ALIAS;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	init_fixture(&fixture);
	fixture.ssa.var_info[0].guarded_reference = true;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	init_fixture(&fixture);
	fixture.ssa.var_info[0].type = MAY_BE_LONG | MAY_BE_INDIRECT;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	init_fixture(&fixture);
	fixture.ssa.var_info[0].range.min = ZEND_LONG_MAX;
	fixture.ssa.var_info[0].range.max = ZEND_LONG_MIN;
	fixture.ssa.var_info[0].range.underflow = true;
	assert(initialize(&fixture, &source, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_value_fact_at(&source, 0, &fact));
	assert((fact.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) == 0);
	assert(fact.provenance == ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS);

	init_fixture(&fixture);
	fixture.ssa.var_info[0].range.min = 5;
	fixture.ssa.var_info[0].range.max = 4;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_REJECTED,
		ZEND_MIRL_CONTRADICTORY_FACT);
}

static void test_stable_deferrals(void)
{
	frontend_fixture fixture;

	init_literal_fixture(&fixture);
	Z_TYPE_INFO(fixture.literals[0]) = IS_STRING;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	init_fixture(&fixture);
	fixture.opcodes[0].opcode = ZEND_JMP;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);

	init_fixture(&fixture);
	fixture.opcodes[0].opcode = ZEND_DO_FCALL;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);

	init_fixture(&fixture);
	fixture.opcodes[0].opcode = ZEND_ASSIGN_REF;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	init_fixture(&fixture);
	fixture.op_array.fn_flags |= ZEND_ACC_RETURN_REFERENCE;
	assert_failure_atomic(
		&fixture, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
}

int main(void)
{
	test_source_view_and_ids();
	test_exact_scalar_fact_mapping();
	test_literal_canonicalization();
	test_pass_two_literal_address();
	test_malformed_sources();
	test_invalid_stable_ids();
	test_conservative_facts();
	test_stable_deferrals();
	puts("frontend source tests: ok");
	return 0;
}
