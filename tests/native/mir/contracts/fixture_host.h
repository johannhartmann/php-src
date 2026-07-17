/* Test-only static-array host for the frozen ZNMIR view/mutator contract. */

#ifndef ZEND_MIR_TEST_FIXTURE_HOST_H
#define ZEND_MIR_TEST_FIXTURE_HOST_H

#include "Zend/Native/MIR/zend_mir.h"

#define ZEND_MIR_FIXTURE_MAX_FUNCTIONS 8
#define ZEND_MIR_FIXTURE_MAX_BLOCKS 32
#define ZEND_MIR_FIXTURE_MAX_VALUES 128
#define ZEND_MIR_FIXTURE_MAX_CONSTANTS 128
#define ZEND_MIR_FIXTURE_MAX_INSTRUCTIONS 128
#define ZEND_MIR_FIXTURE_MAX_FRAME_STATES 32
#define ZEND_MIR_FIXTURE_MAX_SOURCE_POSITIONS 64
#define ZEND_MIR_FIXTURE_MAX_FRAME_SLOTS 128
#define ZEND_MIR_FIXTURE_MAX_ROOTS 128
#define ZEND_MIR_FIXTURE_MAX_CLEANUPS 128
#define ZEND_MIR_FIXTURE_MAX_OPERANDS 256
#define ZEND_MIR_FIXTURE_MAX_EDGES 64

typedef struct _zend_mir_fixture_operand {
	zend_mir_instruction_id instruction_id;
	zend_mir_value_id value_id;
} zend_mir_fixture_operand;

typedef struct _zend_mir_fixture_edge {
	zend_mir_block_id from;
	zend_mir_block_id to;
} zend_mir_fixture_edge;

typedef struct _zend_mir_fixture_host {
	zend_mir_module_id module_id;
	zend_mir_function_record functions[ZEND_MIR_FIXTURE_MAX_FUNCTIONS];
	zend_mir_block_record blocks[ZEND_MIR_FIXTURE_MAX_BLOCKS];
	zend_mir_value_record values[ZEND_MIR_FIXTURE_MAX_VALUES];
	zend_mir_constant_record constants[ZEND_MIR_FIXTURE_MAX_CONSTANTS];
	zend_mir_instruction_record instructions[ZEND_MIR_FIXTURE_MAX_INSTRUCTIONS];
	zend_mir_frame_state_ref frame_states[ZEND_MIR_FIXTURE_MAX_FRAME_STATES];
	zend_mir_source_position_ref source_positions[ZEND_MIR_FIXTURE_MAX_SOURCE_POSITIONS];
	zend_mir_frame_slot_ref frame_slots[ZEND_MIR_FIXTURE_MAX_FRAME_SLOTS];
	uint32_t roots[ZEND_MIR_FIXTURE_MAX_ROOTS];
	zend_mir_cleanup_ref cleanups[ZEND_MIR_FIXTURE_MAX_CLEANUPS];
	zend_mir_fixture_operand operands[ZEND_MIR_FIXTURE_MAX_OPERANDS];
	zend_mir_fixture_edge edges[ZEND_MIR_FIXTURE_MAX_EDGES];
	bool sealed[ZEND_MIR_FIXTURE_MAX_FUNCTIONS];
	uint32_t function_count;
	uint32_t block_count;
	uint32_t value_count;
	uint32_t constant_count;
	uint32_t instruction_count;
	uint32_t frame_state_count;
	uint32_t source_position_count;
	uint32_t frame_slot_count;
	uint32_t root_count;
	uint32_t cleanup_count;
	uint32_t operand_count;
	uint32_t edge_count;
	zend_mir_diagnostic_sink diagnostics;
	zend_mir_view view;
	zend_mir_mutator mutator;
} zend_mir_fixture_host;

void zend_mir_fixture_host_init(zend_mir_fixture_host *host, zend_mir_module_id module_id);

#endif /* ZEND_MIR_TEST_FIXTURE_HOST_H */
