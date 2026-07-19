#include <stdlib.h>
#include <string.h>

#include "zend_mir_control_flow_internal.h"

static uint32_t zend_mir_cf_block_count(const void *context)
{
	const zend_mir_control_flow_map_storage *storage = context;
	return storage == NULL ? 0 : storage->block_count;
}

static bool zend_mir_cf_block_at(const void *context, uint32_t index,
	zend_mir_control_flow_block_mapping *out)
{
	const zend_mir_control_flow_map_storage *storage = context;
	if (storage == NULL || out == NULL || index >= storage->block_count) {
		return false;
	}
	*out = storage->blocks[index];
	return true;
}

static uint32_t zend_mir_cf_edge_count(const void *context)
{
	const zend_mir_control_flow_map_storage *storage = context;
	return storage == NULL ? 0 : storage->edge_count;
}

static bool zend_mir_cf_edge_at(const void *context, uint32_t index,
	zend_mir_control_flow_edge_mapping *out)
{
	const zend_mir_control_flow_map_storage *storage = context;
	if (storage == NULL || out == NULL || index >= storage->edge_count) {
		return false;
	}
	*out = storage->edges[index];
	return true;
}

static uint32_t zend_mir_cf_phi_count(const void *context)
{
	const zend_mir_control_flow_map_storage *storage = context;
	return storage == NULL ? 0 : storage->phi_count;
}

static bool zend_mir_cf_phi_at(const void *context, uint32_t index,
	zend_mir_control_flow_phi_mapping *out)
{
	const zend_mir_control_flow_map_storage *storage = context;
	if (storage == NULL || out == NULL || index >= storage->phi_count) {
		return false;
	}
	*out = storage->phis[index];
	return true;
}

static void *zend_mir_cf_allocate(uint32_t count, size_t size)
{
	if (count == 0) {
		return NULL;
	}
	if ((size_t) count > SIZE_MAX / size) {
		return NULL;
	}
	return calloc(count, size);
}

bool zend_mir_control_flow_map_storage_init(
	zend_mir_control_flow_map_storage *storage,
	uint32_t block_capacity, uint32_t edge_capacity, uint32_t phi_capacity)
{
	if (storage == NULL) {
		return false;
	}
	memset(storage, 0, sizeof(*storage));
	storage->blocks = zend_mir_cf_allocate(block_capacity, sizeof(*storage->blocks));
	storage->edges = zend_mir_cf_allocate(edge_capacity, sizeof(*storage->edges));
	storage->phis = zend_mir_cf_allocate(phi_capacity, sizeof(*storage->phis));
	if ((block_capacity != 0 && storage->blocks == NULL)
			|| (edge_capacity != 0 && storage->edges == NULL)
			|| (phi_capacity != 0 && storage->phis == NULL)) {
		zend_mir_control_flow_map_storage_destroy(storage);
		return false;
	}
	storage->block_capacity = block_capacity;
	storage->edge_capacity = edge_capacity;
	storage->phi_capacity = phi_capacity;
	storage->public_map.contract_version = ZEND_MIR_W04_CONTRACT_VERSION;
	storage->public_map.context = storage;
	storage->public_map.block_count = zend_mir_cf_block_count;
	storage->public_map.block_at = zend_mir_cf_block_at;
	storage->public_map.edge_count = zend_mir_cf_edge_count;
	storage->public_map.edge_at = zend_mir_cf_edge_at;
	storage->public_map.phi_count = zend_mir_cf_phi_count;
	storage->public_map.phi_at = zend_mir_cf_phi_at;
	return true;
}

void zend_mir_control_flow_map_storage_destroy(
	zend_mir_control_flow_map_storage *storage)
{
	if (storage == NULL) {
		return;
	}
	free(storage->phis);
	free(storage->edges);
	free(storage->blocks);
	memset(storage, 0, sizeof(*storage));
}

bool zend_mir_control_flow_map_add_block(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_block_mapping *mapping)
{
	uint32_t i;
	if (storage == NULL || mapping == NULL
			|| storage->block_count >= storage->block_capacity
			|| !zend_mir_id_is_valid(mapping->source_block_id)
			|| !zend_mir_id_is_valid(mapping->mir_block_id)) {
		return false;
	}
	for (i = 0; i < storage->block_count; i++) {
		if (storage->blocks[i].source_block_id == mapping->source_block_id
				|| storage->blocks[i].mir_block_id == mapping->mir_block_id) {
			return false;
		}
	}
	storage->blocks[storage->block_count++] = *mapping;
	return true;
}

bool zend_mir_control_flow_map_add_edge(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_edge_mapping *mapping)
{
	uint32_t i;
	if (storage == NULL || mapping == NULL
			|| storage->edge_count >= storage->edge_capacity
			|| !zend_mir_id_is_valid(mapping->source_edge_id)
			|| !zend_mir_id_is_valid(mapping->mir_from_block_id)
			|| !zend_mir_id_is_valid(mapping->mir_to_block_id)
			|| !zend_mir_id_is_valid(mapping->terminator_instruction_id)
			|| mapping->mir_successor_index > 1) {
		return false;
	}
	for (i = 0; i < storage->edge_count; i++) {
		if (storage->edges[i].source_edge_id == mapping->source_edge_id) {
			return false;
		}
	}
	storage->edges[storage->edge_count++] = *mapping;
	return true;
}

bool zend_mir_control_flow_map_add_phi(
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_control_flow_phi_mapping *mapping)
{
	uint32_t i;
	if (storage == NULL || mapping == NULL
			|| storage->phi_count >= storage->phi_capacity
			|| !zend_mir_id_is_valid(mapping->source_phi_id)
			|| !zend_mir_id_is_valid(mapping->mir_phi_instruction_id)
			|| !zend_mir_id_is_valid(mapping->mir_result_value_id)) {
		return false;
	}
	for (i = 0; i < storage->phi_count; i++) {
		if (storage->phis[i].source_phi_id == mapping->source_phi_id) {
			return false;
		}
	}
	storage->phis[storage->phi_count++] = *mapping;
	return true;
}

bool zend_mir_control_flow_map_find_block(
	const zend_mir_control_flow_map *map,
	zend_mir_source_block_id source_block_id,
	zend_mir_block_id *mir_block_id)
{
	uint32_t i;
	if (map == NULL || mir_block_id == NULL || map->block_count == NULL
			|| map->block_at == NULL) {
		return false;
	}
	for (i = 0; i < map->block_count(map->context); i++) {
		zend_mir_control_flow_block_mapping mapping;
		if (!map->block_at(map->context, i, &mapping)) {
			return false;
		}
		if (mapping.source_block_id == source_block_id) {
			*mir_block_id = mapping.mir_block_id;
			return true;
		}
	}
	return false;
}
