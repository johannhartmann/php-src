#ifndef ZEND_MIR_CONTROL_FLOW_INTERNAL_H
#define ZEND_MIR_CONTROL_FLOW_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "../zend_mir_control_flow.h"

typedef struct _zend_mir_control_flow_cycle_graph {
	const void *context;
	uint32_t (*block_count)(const void *context);
	bool (*block_at)(const void *context, uint32_t index,
		bool *irreducible);
	uint32_t (*edge_count)(const void *context);
	bool (*edge_at)(const void *context, uint32_t index,
		uint32_t *from_block_id, uint32_t *to_block_id,
		bool *requires_statepoint);
} zend_mir_control_flow_cycle_graph;

typedef struct _zend_mir_control_flow_cycle_analysis {
	uint8_t *edge_statepoints;
	uint32_t edge_count;
	bool has_irreducible_cycle;
} zend_mir_control_flow_cycle_analysis;

typedef struct _zend_mir_control_flow_map_storage {
	zend_mir_control_flow_map public_map;
	zend_mir_control_flow_cycle_analysis cycle_analysis;
	zend_mir_control_flow_block_mapping *blocks;
	zend_mir_control_flow_edge_mapping *edges;
	zend_mir_control_flow_phi_mapping *phis;
	zend_mir_block_id *source_blocks;
	uint8_t *source_edges;
	uint8_t *source_phis;
	uint32_t block_count;
	uint32_t edge_count;
	uint32_t phi_count;
	uint32_t block_capacity;
	uint32_t edge_capacity;
	uint32_t phi_capacity;
} zend_mir_control_flow_map_storage;

bool zend_mir_control_flow_map_storage_init(
	zend_mir_control_flow_map_storage *storage,
	uint32_t block_capacity, uint32_t edge_capacity, uint32_t phi_capacity);
void zend_mir_control_flow_map_storage_destroy(
	zend_mir_control_flow_map_storage *storage);
bool zend_mir_control_flow_cycle_analysis_init(
	zend_mir_control_flow_cycle_analysis *analysis,
	const zend_mir_control_flow_cycle_graph *graph);
void zend_mir_control_flow_cycle_analysis_destroy(
	zend_mir_control_flow_cycle_analysis *analysis);
bool zend_mir_control_flow_cycle_edge_requires_statepoint(
	const zend_mir_control_flow_cycle_analysis *analysis,
	zend_mir_source_edge_id edge_id);
bool zend_mir_control_flow_map_add_block(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_block_mapping *mapping);
bool zend_mir_control_flow_map_add_edge(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_edge_mapping *mapping);
bool zend_mir_control_flow_map_add_phi(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_phi_mapping *mapping);
bool zend_mir_control_flow_map_find_block(
	const zend_mir_control_flow_map *map,
	zend_mir_source_block_id source_block_id,
	zend_mir_block_id *mir_block_id);

#endif /* ZEND_MIR_CONTROL_FLOW_INTERNAL_H */
