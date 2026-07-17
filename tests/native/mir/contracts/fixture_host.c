#include "fixture_host.h"

#include <string.h>

static bool zend_mir_fixture_find_function(
		const zend_mir_fixture_host *host, zend_mir_function_id id, uint32_t *index)
{
	uint32_t i;

	for (i = 0; i < host->function_count; i++) {
		if (host->functions[i].id == id) {
			*index = i;
			return true;
		}
	}
	return false;
}

static bool zend_mir_fixture_find_block_function(const zend_mir_fixture_host *host,
		zend_mir_block_id id, zend_mir_function_id *function_id)
{
	uint32_t i;

	for (i = 0; i < host->block_count; i++) {
		if (host->blocks[i].id == id) {
			*function_id = host->blocks[i].function_id;
			return true;
		}
	}
	return false;
}

static bool zend_mir_fixture_find_block(const zend_mir_fixture_host *host, zend_mir_block_id id)
{
	zend_mir_function_id ignored;

	return zend_mir_fixture_find_block_function(host, id, &ignored);
}

static bool zend_mir_fixture_find_instruction(
		const zend_mir_fixture_host *host, zend_mir_instruction_id id)
{
	uint32_t i;

	for (i = 0; i < host->instruction_count; i++) {
		if (host->instructions[i].id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_fixture_find_value(const zend_mir_fixture_host *host, zend_mir_value_id id)
{
	uint32_t i;

	for (i = 0; i < host->value_count; i++) {
		if (host->values[i].id == id) {
			return true;
		}
	}
	return false;
}

static zend_mir_module_id zend_mir_fixture_module_id(const void *context)
{
	return ((const zend_mir_fixture_host *) context)->module_id;
}

#define ZEND_MIR_FIXTURE_COUNT_FN(name, field) \
	static uint32_t name(const void *context) \
	{ \
		return ((const zend_mir_fixture_host *) context)->field; \
	}

ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_function_count, function_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_block_count, block_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_instruction_count, instruction_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_value_count, value_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_constant_count, constant_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_frame_state_count, frame_state_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_source_position_count, source_position_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_source_map_count, source_map_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_frame_slot_count, frame_slot_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_root_count, root_count)
ZEND_MIR_FIXTURE_COUNT_FN(zend_mir_fixture_cleanup_count, cleanup_count)

#undef ZEND_MIR_FIXTURE_COUNT_FN

#define ZEND_MIR_FIXTURE_AT_FN(name, type, field, count_field) \
	static bool name(const void *context, uint32_t index, type *out) \
	{ \
		const zend_mir_fixture_host *host = (const zend_mir_fixture_host *) context; \
		if (out == NULL || index >= host->count_field) { \
			return false; \
		} \
		*out = host->field[index]; \
		return true; \
	}

ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_function_at, zend_mir_function_record, functions, function_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_block_at, zend_mir_block_record, blocks, block_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_instruction_at, zend_mir_instruction_record, instructions, instruction_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_value_at, zend_mir_value_record, values, value_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_constant_at, zend_mir_constant_record, constants, constant_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_frame_state_at, zend_mir_frame_state_ref, frame_states, frame_state_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_source_position_at, zend_mir_source_position_ref,
	source_positions, source_position_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_source_map_at, zend_mir_source_map_ref,
	source_maps, source_map_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_frame_slot_at, zend_mir_frame_slot_ref, frame_slots, frame_slot_count)
ZEND_MIR_FIXTURE_AT_FN(zend_mir_fixture_cleanup_at, zend_mir_cleanup_ref, cleanups, cleanup_count)

#undef ZEND_MIR_FIXTURE_AT_FN

static bool zend_mir_fixture_root_at(const void *context, uint32_t index, uint32_t *slot_id_out)
{
	const zend_mir_fixture_host *host = (const zend_mir_fixture_host *) context;

	if (slot_id_out == NULL || index >= host->root_count) {
		return false;
	}
	*slot_id_out = host->roots[index];
	return true;
}

static uint32_t zend_mir_fixture_instruction_operand_count(
		const void *context, zend_mir_instruction_id instruction_id)
{
	const zend_mir_fixture_host *host = (const zend_mir_fixture_host *) context;
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < host->operand_count; i++) {
		if (host->operands[i].instruction_id == instruction_id) {
			count++;
		}
	}
	return count;
}

static bool zend_mir_fixture_instruction_operand_at(const void *context,
		zend_mir_instruction_id instruction_id, uint32_t index, zend_mir_value_id *out)
{
	const zend_mir_fixture_host *host = (const zend_mir_fixture_host *) context;
	uint32_t found = 0;
	uint32_t i;

	if (out == NULL) {
		return false;
	}
	for (i = 0; i < host->operand_count; i++) {
		if (host->operands[i].instruction_id == instruction_id) {
			if (found == index) {
				*out = host->operands[i].value_id;
				return true;
			}
			found++;
		}
	}
	return false;
}

static uint32_t zend_mir_fixture_edge_count(
		const zend_mir_fixture_host *host, zend_mir_block_id block_id, bool successor)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < host->edge_count; i++) {
		if ((successor ? host->edges[i].from : host->edges[i].to) == block_id) {
			count++;
		}
	}
	return count;
}

static bool zend_mir_fixture_edge_at(const zend_mir_fixture_host *host,
		zend_mir_block_id block_id, uint32_t index, bool successor, zend_mir_block_id *out)
{
	uint32_t found = 0;
	uint32_t i;

	if (out == NULL) {
		return false;
	}
	for (i = 0; i < host->edge_count; i++) {
		if ((successor ? host->edges[i].from : host->edges[i].to) == block_id) {
			if (found == index) {
				*out = successor ? host->edges[i].to : host->edges[i].from;
				return true;
			}
			found++;
		}
	}
	return false;
}

static uint32_t zend_mir_fixture_successor_count(const void *context, zend_mir_block_id block_id)
{
	return zend_mir_fixture_edge_count((const zend_mir_fixture_host *) context, block_id, true);
}

static bool zend_mir_fixture_successor_at(
		const void *context, zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	return zend_mir_fixture_edge_at(
		(const zend_mir_fixture_host *) context, block_id, index, true, out);
}

static uint32_t zend_mir_fixture_predecessor_count(const void *context, zend_mir_block_id block_id)
{
	return zend_mir_fixture_edge_count((const zend_mir_fixture_host *) context, block_id, false);
}

static bool zend_mir_fixture_predecessor_at(
		const void *context, zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	return zend_mir_fixture_edge_at(
		(const zend_mir_fixture_host *) context, block_id, index, false, out);
}

static bool zend_mir_fixture_add_function(
		void *context, zend_mir_symbol_id symbol_id, zend_mir_function_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_function_record *record;

	if (out == NULL || host->function_count >= ZEND_MIR_FIXTURE_MAX_FUNCTIONS) {
		return false;
	}
	record = &host->functions[host->function_count];
	record->id = host->function_count;
	record->symbol_id = symbol_id;
	record->entry_block_id = ZEND_MIR_ID_INVALID;
	record->flags = 0;
	*out = record->id;
	host->function_count++;
	return true;
}

static bool zend_mir_fixture_add_block(
		void *context, zend_mir_function_id function_id, zend_mir_block_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_block_record *record;
	uint32_t ignored;

	if (out == NULL || host->block_count >= ZEND_MIR_FIXTURE_MAX_BLOCKS
			|| !zend_mir_fixture_find_function(host, function_id, &ignored)) {
		return false;
	}
	record = &host->blocks[host->block_count];
	record->id = host->block_count;
	record->function_id = function_id;
	*out = record->id;
	host->block_count++;
	return true;
}

static bool zend_mir_fixture_set_entry_block(
		void *context, zend_mir_function_id function_id, zend_mir_block_id block_id)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	uint32_t index;
	zend_mir_function_id block_function_id;

	if (!zend_mir_fixture_find_function(host, function_id, &index)
			|| !zend_mir_fixture_find_block_function(host, block_id, &block_function_id)
			|| block_function_id != function_id) {
		return false;
	}
	host->functions[index].entry_block_id = block_id;
	return true;
}

static bool zend_mir_fixture_add_value(void *context, zend_mir_value_id requested_id,
		zend_mir_representation representation, zend_mir_ownership_state ownership)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_value_record *record;

	if (!zend_mir_id_is_valid(requested_id)
			|| representation >= ZEND_MIR_REPRESENTATION_COUNT
			|| ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT
			|| host->value_count >= ZEND_MIR_FIXTURE_MAX_VALUES
			|| zend_mir_fixture_find_value(host, requested_id)) {
		return false;
	}
	record = &host->values[host->value_count++];
	record->id = requested_id;
	record->representation = representation;
	record->ownership = ownership;
	return true;
}

static bool zend_mir_fixture_add_constant(
		void *context, const zend_mir_constant_record *requested)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	uint32_t i;

	if (requested == NULL || host->constant_count >= ZEND_MIR_FIXTURE_MAX_CONSTANTS
			|| !zend_mir_fixture_find_value(host, requested->value_id)
			|| requested->representation >= ZEND_MIR_REPRESENTATION_COUNT
			|| requested->kind >= ZEND_MIR_CONSTANT_KIND_COUNT) {
		return false;
	}
	for (i = 0; i < host->constant_count; i++) {
		if (host->constants[i].value_id == requested->value_id) {
			return false;
		}
	}
	host->constants[host->constant_count++] = *requested;
	return true;
}

static bool zend_mir_fixture_add_instruction(void *context,
		const zend_mir_instruction_record *requested, zend_mir_instruction_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_instruction_record record;

	if (requested == NULL || out == NULL
			|| host->instruction_count >= ZEND_MIR_FIXTURE_MAX_INSTRUCTIONS
			|| !zend_mir_fixture_find_block(host, requested->block_id)
			|| requested->opcode >= ZEND_MIR_OPCODE_COUNT) {
		return false;
	}
	record = *requested;
	record.id = host->instruction_count;
	host->instructions[host->instruction_count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_fixture_add_operand(
		void *context, zend_mir_instruction_id instruction_id, zend_mir_value_id value_id)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_fixture_operand *operand;

	if (host->operand_count >= ZEND_MIR_FIXTURE_MAX_OPERANDS
			|| !zend_mir_fixture_find_instruction(host, instruction_id)
			|| !zend_mir_fixture_find_value(host, value_id)) {
		return false;
	}
	operand = &host->operands[host->operand_count++];
	operand->instruction_id = instruction_id;
	operand->value_id = value_id;
	return true;
}

static bool zend_mir_fixture_add_edge(
		void *context, zend_mir_block_id from, zend_mir_block_id to)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_fixture_edge *edge;
	zend_mir_function_id from_function;
	zend_mir_function_id to_function;
	uint32_t i;

	if (host->edge_count >= ZEND_MIR_FIXTURE_MAX_EDGES
			|| !zend_mir_fixture_find_block_function(host, from, &from_function)
			|| !zend_mir_fixture_find_block_function(host, to, &to_function)
			|| from_function != to_function) {
		return false;
	}
	for (i = 0; i < host->edge_count; i++) {
		if (host->edges[i].from == from && host->edges[i].to == to) {
			return false;
		}
	}
	edge = &host->edges[host->edge_count++];
	edge->from = from;
	edge->to = to;
	return true;
}

static bool zend_mir_fixture_add_source_position(void *context,
		const zend_mir_source_position_ref *requested, zend_mir_source_position_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_source_position_ref record;

	if (requested == NULL || out == NULL
			|| host->source_position_count >= ZEND_MIR_FIXTURE_MAX_SOURCE_POSITIONS) {
		return false;
	}
	record = *requested;
	record.id = host->source_position_count;
	host->source_positions[host->source_position_count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_fixture_add_frame_slot(
		void *context, const zend_mir_frame_slot_ref *slot, uint32_t *index_out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;

	if (slot == NULL || index_out == NULL
			|| host->frame_slot_count >= ZEND_MIR_FIXTURE_MAX_FRAME_SLOTS) {
		return false;
	}
	*index_out = host->frame_slot_count;
	host->frame_slots[host->frame_slot_count++] = *slot;
	return true;
}

static bool zend_mir_fixture_has_slot(const zend_mir_fixture_host *host, uint32_t slot_id)
{
	uint32_t i;

	for (i = 0; i < host->frame_slot_count; i++) {
		if (host->frame_slots[i].slot_id == slot_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_fixture_add_root(void *context, uint32_t slot_id, uint32_t *index_out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;

	if (index_out == NULL || host->root_count >= ZEND_MIR_FIXTURE_MAX_ROOTS
			|| !zend_mir_fixture_has_slot(host, slot_id)) {
		return false;
	}
	*index_out = host->root_count;
	host->roots[host->root_count++] = slot_id;
	return true;
}

static bool zend_mir_fixture_add_cleanup(
		void *context, const zend_mir_cleanup_ref *cleanup, uint32_t *index_out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;

	if (cleanup == NULL || index_out == NULL
			|| host->cleanup_count >= ZEND_MIR_FIXTURE_MAX_CLEANUPS
			|| !zend_mir_fixture_has_slot(host, cleanup->slot_id)) {
		return false;
	}
	*index_out = host->cleanup_count;
	host->cleanups[host->cleanup_count++] = *cleanup;
	return true;
}

static bool zend_mir_fixture_span_is_valid(zend_mir_span span, uint32_t count)
{
	return span.offset <= count && span.count <= count - span.offset;
}

static bool zend_mir_fixture_add_frame_state(void *context,
		const zend_mir_frame_state_ref *requested, zend_mir_frame_state_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_frame_state_ref record;

	if (requested == NULL || out == NULL
			|| host->frame_state_count >= ZEND_MIR_FIXTURE_MAX_FRAME_STATES
			|| !zend_mir_fixture_span_is_valid(requested->slots, host->frame_slot_count)
			|| !zend_mir_fixture_span_is_valid(requested->roots, host->root_count)
			|| !zend_mir_fixture_span_is_valid(requested->cleanup_obligations, host->cleanup_count)) {
		return false;
	}
	record = *requested;
	record.id = host->frame_state_count;
	host->frame_states[host->frame_state_count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_fixture_add_source_map(void *context,
		const zend_mir_source_map_ref *requested, zend_mir_source_map_id *out)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	zend_mir_source_map_ref record;
	zend_mir_frame_state_ref owner;

	if (requested == NULL || out == NULL
			|| host->source_map_count >= ZEND_MIR_FIXTURE_MAX_SOURCE_MAPS
			|| !zend_mir_id_is_valid(requested->source_position_id)
			|| requested->source_position_id >= host->source_position_count
			|| !zend_mir_id_is_valid(requested->owner_frame_id)
			|| requested->owner_frame_id >= host->frame_state_count
			|| !zend_mir_id_is_valid(requested->op_array_id)
			|| requested->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT) {
		return false;
	}
	owner = host->frame_states[requested->owner_frame_id];
	if (owner.opline_index != requested->opline_index
			|| owner.opline_phase != requested->opline_phase) {
		return false;
	}
	record = *requested;
	record.id = host->source_map_count;
	host->source_maps[host->source_map_count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_fixture_seal_function(void *context, zend_mir_function_id function_id)
{
	zend_mir_fixture_host *host = (zend_mir_fixture_host *) context;
	uint32_t index;

	if (!zend_mir_fixture_find_function(host, function_id, &index)
			|| !zend_mir_id_is_valid(host->functions[index].entry_block_id)) {
		return false;
	}
	host->sealed[index] = true;
	return true;
}

void zend_mir_fixture_host_init(zend_mir_fixture_host *host, zend_mir_module_id module_id)
{
	memset(host, 0, sizeof(*host));
	host->module_id = module_id;

	host->view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	host->view.context = host;
	host->view.module_id = zend_mir_fixture_module_id;
	host->view.function_count = zend_mir_fixture_function_count;
	host->view.function_at = zend_mir_fixture_function_at;
	host->view.block_count = zend_mir_fixture_block_count;
	host->view.block_at = zend_mir_fixture_block_at;
	host->view.instruction_count = zend_mir_fixture_instruction_count;
	host->view.instruction_at = zend_mir_fixture_instruction_at;
	host->view.value_count = zend_mir_fixture_value_count;
	host->view.value_at = zend_mir_fixture_value_at;
	host->view.constant_count = zend_mir_fixture_constant_count;
	host->view.constant_at = zend_mir_fixture_constant_at;
	host->view.frame_state_count = zend_mir_fixture_frame_state_count;
	host->view.frame_state_at = zend_mir_fixture_frame_state_at;
	host->view.source_position_count = zend_mir_fixture_source_position_count;
	host->view.source_position_at = zend_mir_fixture_source_position_at;
	host->view.frame_slot_count = zend_mir_fixture_frame_slot_count;
	host->view.frame_slot_at = zend_mir_fixture_frame_slot_at;
	host->view.root_count = zend_mir_fixture_root_count;
	host->view.root_at = zend_mir_fixture_root_at;
	host->view.cleanup_count = zend_mir_fixture_cleanup_count;
	host->view.cleanup_at = zend_mir_fixture_cleanup_at;
	host->view.instruction_operand_count = zend_mir_fixture_instruction_operand_count;
	host->view.instruction_operand_at = zend_mir_fixture_instruction_operand_at;
	host->view.successor_count = zend_mir_fixture_successor_count;
	host->view.successor_at = zend_mir_fixture_successor_at;
	host->view.predecessor_count = zend_mir_fixture_predecessor_count;
	host->view.predecessor_at = zend_mir_fixture_predecessor_at;
	host->view.source_map_count = zend_mir_fixture_source_map_count;
	host->view.source_map_at = zend_mir_fixture_source_map_at;

	host->mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	host->mutator.context = host;
	host->mutator.diagnostics = &host->diagnostics;
	host->mutator.add_function = zend_mir_fixture_add_function;
	host->mutator.add_block = zend_mir_fixture_add_block;
	host->mutator.set_entry_block = zend_mir_fixture_set_entry_block;
	host->mutator.add_value = zend_mir_fixture_add_value;
	host->mutator.add_constant = zend_mir_fixture_add_constant;
	host->mutator.add_instruction = zend_mir_fixture_add_instruction;
	host->mutator.add_operand = zend_mir_fixture_add_operand;
	host->mutator.add_edge = zend_mir_fixture_add_edge;
	host->mutator.add_source_position = zend_mir_fixture_add_source_position;
	host->mutator.add_frame_slot = zend_mir_fixture_add_frame_slot;
	host->mutator.add_root = zend_mir_fixture_add_root;
	host->mutator.add_cleanup = zend_mir_fixture_add_cleanup;
	host->mutator.add_frame_state = zend_mir_fixture_add_frame_state;
	host->mutator.seal_function = zend_mir_fixture_seal_function;
	host->mutator.add_source_map = zend_mir_fixture_add_source_map;
}
