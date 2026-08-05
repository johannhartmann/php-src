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

typedef struct _zend_mir_cf_dfs_frame {
	uint32_t block_id;
	uint32_t next_edge;
} zend_mir_cf_dfs_frame;

void zend_mir_control_flow_cycle_analysis_destroy(
	zend_mir_control_flow_cycle_analysis *analysis)
{
	if (analysis == NULL) {
		return;
	}
	free(analysis->edge_statepoints);
	memset(analysis, 0, sizeof(*analysis));
}

bool zend_mir_control_flow_cycle_analysis_init(
	zend_mir_control_flow_cycle_analysis *analysis,
	const zend_mir_control_flow_cycle_graph *graph)
{
	uint32_t *out_offsets = NULL;
	uint32_t *in_offsets = NULL;
	uint32_t *out_edges = NULL;
	uint32_t *in_edges = NULL;
	uint32_t *edge_from = NULL;
	uint32_t *edge_to = NULL;
	uint32_t *cursors = NULL;
	uint32_t *order = NULL;
	uint32_t *components = NULL;
	uint32_t *component_sizes = NULL;
	uint8_t *component_irreducible = NULL;
	uint8_t *seen = NULL;
	zend_mir_cf_dfs_frame *frames = NULL;
	uint32_t *stack = NULL;
	uint32_t block_count;
	uint32_t edge_count;
	uint32_t order_count = 0;
	uint32_t component_count = 0;
	uint32_t i;
	bool success = false;

	if (analysis == NULL || graph == NULL
			|| graph->block_count == NULL || graph->block_at == NULL
			|| graph->edge_count == NULL || graph->edge_at == NULL) {
		return false;
	}
	memset(analysis, 0, sizeof(*analysis));
	block_count = graph->block_count(graph->context);
	edge_count = graph->edge_count(graph->context);
	if (block_count == 0 || block_count == UINT32_MAX) {
		return false;
	}
	analysis->edge_count = edge_count;
	analysis->edge_statepoints = zend_mir_cf_allocate(
		edge_count, sizeof(*analysis->edge_statepoints));
	out_offsets = zend_mir_cf_allocate(
		block_count + 1, sizeof(*out_offsets));
	in_offsets = zend_mir_cf_allocate(
		block_count + 1, sizeof(*in_offsets));
	order = zend_mir_cf_allocate(block_count, sizeof(*order));
	components = zend_mir_cf_allocate(block_count, sizeof(*components));
	component_sizes = zend_mir_cf_allocate(
		block_count, sizeof(*component_sizes));
	component_irreducible = zend_mir_cf_allocate(
		block_count, sizeof(*component_irreducible));
	seen = zend_mir_cf_allocate(block_count, sizeof(*seen));
	frames = zend_mir_cf_allocate(block_count, sizeof(*frames));
	stack = zend_mir_cf_allocate(block_count, sizeof(*stack));
	if ((edge_count != 0 && analysis->edge_statepoints == NULL)
			|| out_offsets == NULL || in_offsets == NULL || order == NULL
			|| components == NULL || component_sizes == NULL
			|| component_irreducible == NULL || seen == NULL
			|| frames == NULL || stack == NULL) {
		goto done;
	}
	for (i = 0; i < block_count; i++) {
		bool irreducible;
		components[i] = UINT32_MAX;
		if (!graph->block_at(graph->context, i, &irreducible)) {
			goto done;
		}
	}
	if (edge_count != 0) {
		out_edges = zend_mir_cf_allocate(edge_count, sizeof(*out_edges));
		in_edges = zend_mir_cf_allocate(edge_count, sizeof(*in_edges));
		edge_from = zend_mir_cf_allocate(edge_count, sizeof(*edge_from));
		edge_to = zend_mir_cf_allocate(edge_count, sizeof(*edge_to));
		cursors = zend_mir_cf_allocate(
			block_count + 1, sizeof(*cursors));
		if (out_edges == NULL || in_edges == NULL || edge_from == NULL
				|| edge_to == NULL || cursors == NULL) {
			goto done;
		}
	}
	for (i = 0; i < edge_count; i++) {
		bool requires_statepoint;
		if (!graph->edge_at(graph->context, i, &edge_from[i], &edge_to[i],
				&requires_statepoint)
				|| edge_from[i] >= block_count || edge_to[i] >= block_count
				|| out_offsets[edge_from[i] + 1] == UINT32_MAX
				|| in_offsets[edge_to[i] + 1] == UINT32_MAX) {
			goto done;
		}
		out_offsets[edge_from[i] + 1]++;
		in_offsets[edge_to[i] + 1]++;
		analysis->edge_statepoints[i] = requires_statepoint;
	}
	for (i = 1; i <= block_count; i++) {
		if (out_offsets[i] > UINT32_MAX - out_offsets[i - 1]
				|| in_offsets[i] > UINT32_MAX - in_offsets[i - 1]) {
			goto done;
		}
		out_offsets[i] += out_offsets[i - 1];
		in_offsets[i] += in_offsets[i - 1];
	}
	if (edge_count != 0) {
		memcpy(cursors, out_offsets,
			(size_t) block_count * sizeof(*cursors));
		for (i = 0; i < edge_count; i++) {
			out_edges[cursors[edge_from[i]]++] = i;
		}
		memcpy(cursors, in_offsets,
			(size_t) block_count * sizeof(*cursors));
		for (i = 0; i < edge_count; i++) {
			in_edges[cursors[edge_to[i]]++] = i;
		}
	}
	for (i = 0; i < block_count; i++) {
		uint32_t frame_count = 0;
		if (seen[i]) {
			continue;
		}
		seen[i] = 1;
		frames[frame_count++] = (zend_mir_cf_dfs_frame) {
			i, out_offsets[i]
		};
		while (frame_count != 0) {
			zend_mir_cf_dfs_frame *frame = &frames[frame_count - 1];
			uint32_t end = out_offsets[frame->block_id + 1];
			if (frame->next_edge < end) {
				uint32_t edge_id = out_edges[frame->next_edge++];
				uint32_t target = edge_to[edge_id];
				if (!seen[target]) {
					seen[target] = 1;
					frames[frame_count++] = (zend_mir_cf_dfs_frame) {
						target, out_offsets[target]
					};
				}
			} else {
				order[order_count++] = frame->block_id;
				frame_count--;
			}
		}
	}
	memset(seen, 0, (size_t) block_count * sizeof(*seen));
	while (order_count != 0) {
		uint32_t root = order[--order_count];
		uint32_t stack_count = 0;
		if (seen[root]) {
			continue;
		}
		seen[root] = 1;
		components[root] = component_count;
		stack[stack_count++] = root;
		while (stack_count != 0) {
			uint32_t block_id = stack[--stack_count];
			uint32_t edge_index;
			component_sizes[component_count]++;
			for (edge_index = in_offsets[block_id];
				edge_index < in_offsets[block_id + 1]; edge_index++) {
				uint32_t predecessor = edge_from[in_edges[edge_index]];
				if (!seen[predecessor]) {
					seen[predecessor] = 1;
					components[predecessor] = component_count;
					stack[stack_count++] = predecessor;
				}
			}
		}
		component_count++;
	}
	for (i = 0; i < block_count; i++) {
		bool irreducible;
		if (!graph->block_at(graph->context, i, &irreducible)) {
			goto done;
		}
		if (irreducible) {
			component_irreducible[components[i]] = 1;
		}
	}
	for (i = 0; i < edge_count; i++) {
		uint32_t component = components[edge_from[i]];
		if (component == components[edge_to[i]]
				&& component_irreducible[component]
				&& (component_sizes[component] > 1
					|| edge_from[i] == edge_to[i])) {
			analysis->has_irreducible_cycle = true;
			analysis->edge_statepoints[i] = 1;
		}
	}
	success = true;

done:
	free(stack);
	free(frames);
	free(seen);
	free(component_irreducible);
	free(component_sizes);
	free(components);
	free(order);
	free(cursors);
	free(edge_to);
	free(edge_from);
	free(in_edges);
	free(out_edges);
	free(in_offsets);
	free(out_offsets);
	if (!success) {
		zend_mir_control_flow_cycle_analysis_destroy(analysis);
	}
	return success;
}

bool zend_mir_control_flow_cycle_edge_requires_statepoint(
	const zend_mir_control_flow_cycle_analysis *analysis,
	zend_mir_source_edge_id edge_id)
{
	return analysis != NULL && edge_id < analysis->edge_count
		&& analysis->edge_statepoints[edge_id] != 0;
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
	zend_mir_control_flow_cycle_analysis_destroy(&storage->cycle_analysis);
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
			|| mapping->mir_successor_index == UINT32_MAX) {
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
