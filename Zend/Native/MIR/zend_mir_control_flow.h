#ifndef ZEND_MIR_CONTROL_FLOW_H
#define ZEND_MIR_CONTROL_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "zend_mir_ids.h"

typedef struct _zend_mir_control_flow_block_mapping {
	zend_mir_source_block_id source_block_id;
	zend_mir_block_id mir_block_id;
} zend_mir_control_flow_block_mapping;

typedef struct _zend_mir_control_flow_edge_mapping {
	zend_mir_source_edge_id source_edge_id;
	zend_mir_block_id mir_from_block_id;
	zend_mir_block_id mir_to_block_id;
	zend_mir_instruction_id terminator_instruction_id;
	zend_mir_instruction_id edge_statepoint_instruction_id;
	uint32_t mir_successor_index;
} zend_mir_control_flow_edge_mapping;

typedef struct _zend_mir_control_flow_phi_mapping {
	zend_mir_source_phi_id source_phi_id;
	zend_mir_instruction_id mir_phi_instruction_id;
	zend_mir_value_id mir_result_value_id;
} zend_mir_control_flow_phi_mapping;

/*
 * Process-local mapping callbacks are valid only during W04 lowering and
 * stage-3 verification. No mapping record is part of persistent MIR.
 */
typedef struct _zend_mir_control_flow_map {
	uint32_t contract_version;
	const void *context;
	uint32_t (*block_count)(const void *context);
	bool (*block_at)(const void *context, uint32_t index,
		zend_mir_control_flow_block_mapping *out);
	uint32_t (*edge_count)(const void *context);
	bool (*edge_at)(const void *context, uint32_t index,
		zend_mir_control_flow_edge_mapping *out);
	uint32_t (*phi_count)(const void *context);
	bool (*phi_at)(const void *context, uint32_t index,
		zend_mir_control_flow_phi_mapping *out);
} zend_mir_control_flow_map;

#endif /* ZEND_MIR_CONTROL_FLOW_H */
