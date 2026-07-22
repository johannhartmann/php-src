#include "zend_mir_cfg_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static zend_mir_diagnostic_code zend_mir_cfg_diagnostic_code(zend_mir_cfg_status status)
{
	switch (status) {
		case ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED:
			return ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED;
		case ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED:
			return ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED;
		case ZEND_MIR_CFG_STATUS_INVALID_PHI:
			return ZEND_MIR_DIAGNOSTIC_INVALID_PHI;
		case ZEND_MIR_CFG_STATUS_INCOMPATIBLE_CONTRACT:
			return ZEND_MIR_DIAGNOSTIC_UNSUPPORTED_CONTRACT_VERSION;
		case ZEND_MIR_CFG_STATUS_NOT_FOUND:
		case ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT:
			return ZEND_MIR_DIAGNOSTIC_INVALID_ID;
		default:
			return ZEND_MIR_DIAGNOSTIC_INVALID_CFG;
	}
}

bool zend_mir_cfg_size_multiply(size_t count, size_t width, size_t *out)
{
	if (out == NULL || (width != 0 && count > SIZE_MAX / width)) {
		return false;
	}
	*out = count * width;
	return true;
}

void *zend_mir_cfg_allocate(zend_mir_cfg *cfg, size_t count, size_t width,
		size_t alignment, zend_mir_cfg_status *status)
{
	size_t size;
	void *allocation;

	if (status == NULL) {
		return NULL;
	}
	if (cfg == NULL || cfg->allocator.allocate == NULL) {
		*status = ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
		return NULL;
	}
	if (count == 0) {
		*status = ZEND_MIR_CFG_STATUS_OK;
		return NULL;
	}
	if (!zend_mir_cfg_size_multiply(count, width, &size)) {
		*status = ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		return NULL;
	}
	allocation = cfg->allocator.allocate(cfg->allocator.context, size, alignment);
	*status = allocation == NULL ? ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED
		: ZEND_MIR_CFG_STATUS_OK;
	return allocation;
}

void zend_mir_cfg_emit(zend_mir_cfg *cfg, zend_mir_cfg_status status,
		zend_mir_block_id block_id, zend_mir_instruction_id instruction_id,
		const char *message)
{
	zend_mir_diagnostic diagnostic;

	if (cfg == NULL || cfg->diagnostics == NULL) {
		return;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = zend_mir_cfg_diagnostic_code(status);
	diagnostic.severity = status == ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED
		? ZEND_MIR_DIAGNOSTIC_FATAL : ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = cfg->view.module_id != NULL
		? cfg->view.module_id(cfg->view.context) : ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = cfg->function_id;
	diagnostic.location.block_id = block_id;
	diagnostic.location.instruction_id = instruction_id;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	if (message != NULL) {
		(void) snprintf(diagnostic.message, sizeof(diagnostic.message), "%s", message);
	}
	(void) zend_mir_diagnostic_sink_emit(cfg->diagnostics, &diagnostic);
}

int zend_mir_cfg_find_block(const zend_mir_cfg *cfg, zend_mir_block_id block_id)
{
	uint32_t i;

	if (cfg == NULL) {
		return -1;
	}
	for (i = 0; i < cfg->block_count; i++) {
		if (cfg->blocks[i].id == block_id) {
			return (int) i;
		}
	}
	return -1;
}

int zend_mir_cfg_find_instruction(const zend_mir_cfg *cfg,
		zend_mir_instruction_id instruction_id)
{
	uint32_t i;

	if (cfg == NULL) {
		return -1;
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		if (cfg->instructions[i].id == instruction_id) {
			return (int) i;
		}
	}
	return -1;
}

bool zend_mir_cfg_block_is_selected(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id)
{
	int index = zend_mir_cfg_find_block(cfg, block_id);

	return index >= 0 && cfg->blocks[index].function_id == cfg->function_id;
}

static bool zend_mir_cfg_value_exists(const zend_mir_cfg *cfg, zend_mir_value_id value_id)
{
	uint32_t i;
	zend_mir_value_record value;

	if (!zend_mir_id_is_valid(value_id)) {
		return false;
	}
	for (i = 0; i < cfg->source->value_count(cfg->source->context); i++) {
		if (!cfg->source->value_at(cfg->source->context, i, &value)) {
			return false;
		}
		if (value.id == value_id) {
			return true;
		}
	}
	return false;
}

uint32_t zend_mir_cfg_predecessor_count_internal(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < cfg->edge_count; i++) {
		if (cfg->edges[i].to == block_id) {
			count++;
		}
	}
	return count;
}

uint32_t zend_mir_cfg_successor_count_internal(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < cfg->edge_count; i++) {
		if (cfg->edges[i].from == block_id) {
			count++;
		}
	}
	return count;
}

uint32_t zend_mir_cfg_phi_count_internal(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < cfg->instruction_count; i++) {
		if (cfg->instructions[i].block_id == block_id
				&& cfg->instructions[i].opcode == ZEND_MIR_OPCODE_PHI) {
			count++;
		}
	}
	return count;
}

bool zend_mir_cfg_phi_value_at(const zend_mir_cfg *cfg,
		zend_mir_instruction_id instruction_id, uint32_t slot,
		zend_mir_value_id *value_id)
{
	uint32_t found = 0;
	uint32_t i;

	if (value_id == NULL) {
		return false;
	}
	for (i = 0; i < cfg->operand_count; i++) {
		if (cfg->operands[i].instruction_id == instruction_id) {
			if (found == slot) {
				*value_id = cfg->operands[i].value_id;
				return true;
			}
			found++;
		}
	}
	return false;
}

static int zend_mir_cfg_find_edge(const zend_mir_cfg *cfg,
		zend_mir_block_id from, zend_mir_block_id to)
{
	uint32_t i;

	for (i = 0; i < cfg->edge_count; i++) {
		if (cfg->edges[i].from == from && cfg->edges[i].to == to) {
			return (int) i;
		}
	}
	return -1;
}

static bool zend_mir_cfg_edge_slot_exists(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id, uint32_t slot, bool successor)
{
	uint32_t i;

	for (i = 0; i < cfg->edge_count; i++) {
		if ((successor ? cfg->edges[i].from : cfg->edges[i].to) == block_id
				&& (successor ? cfg->edges[i].successor_slot
					: cfg->edges[i].predecessor_slot) == slot) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_cfg_find_function_record(const zend_mir_cfg *cfg,
		zend_mir_function_id function_id, zend_mir_function_record *out)
{
	uint32_t i;

	for (i = 0; i < cfg->source->function_count(cfg->source->context); i++) {
		if (!cfg->source->function_at(cfg->source->context, i, out)) {
			return false;
		}
		if (out->id == function_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_cfg_block_terminator(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id, zend_mir_instruction_record *out,
		uint32_t *instruction_count)
{
	uint32_t count = 0;
	uint32_t i;
	zend_mir_instruction_record last;

	memset(&last, 0, sizeof(last));
	for (i = 0; i < cfg->instruction_count; i++) {
		if (cfg->instructions[i].block_id == block_id) {
			last = cfg->instructions[i];
			count++;
		}
	}
	if (instruction_count != NULL) {
		*instruction_count = count;
	}
	if (count == 0 || !zend_mir_opcode_is_terminator(last.opcode)) {
		return false;
	}
	if (out != NULL) {
		*out = last;
	}
	return true;
}

static uint32_t zend_mir_cfg_expected_successors(zend_mir_opcode opcode)
{
	switch (opcode) {
		case ZEND_MIR_OPCODE_BRANCH:
			return 1;
		case ZEND_MIR_OPCODE_COND_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH:
			return 2;
		case ZEND_MIR_OPCODE_RETURN:
		case ZEND_MIR_OPCODE_THROW:
		case ZEND_MIR_OPCODE_UNREACHABLE:
			return 0;
		default:
			return UINT32_MAX;
	}
}

static zend_mir_module_id zend_mir_cfg_view_module_id(const void *context)
{
	const zend_mir_cfg *cfg = (const zend_mir_cfg *) context;

	return cfg->source->module_id(cfg->source->context);
}

#define ZEND_MIR_CFG_DELEGATE_COUNT(name, member) \
	static uint32_t name(const void *context) \
	{ \
		const zend_mir_cfg *cfg = (const zend_mir_cfg *) context; \
		return cfg->source->member(cfg->source->context); \
	}

ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_function_count, function_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_value_count, value_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_constant_count, constant_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_frame_state_count, frame_state_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_source_position_count, source_position_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_frame_slot_count, frame_slot_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_root_count, root_count)
ZEND_MIR_CFG_DELEGATE_COUNT(zend_mir_cfg_view_cleanup_count, cleanup_count)

#undef ZEND_MIR_CFG_DELEGATE_COUNT

#define ZEND_MIR_CFG_DELEGATE_AT(name, type, member) \
	static bool name(const void *context, uint32_t index, type *out) \
	{ \
		const zend_mir_cfg *cfg = (const zend_mir_cfg *) context; \
		return cfg->source->member(cfg->source->context, index, out); \
	}

ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_function_at, zend_mir_function_record, function_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_value_at, zend_mir_value_record, value_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_constant_at, zend_mir_constant_record, constant_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_frame_state_at, zend_mir_frame_state_ref, frame_state_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_source_position_at, zend_mir_source_position_ref,
	source_position_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_frame_slot_at, zend_mir_frame_slot_ref, frame_slot_at)
ZEND_MIR_CFG_DELEGATE_AT(zend_mir_cfg_view_cleanup_at, zend_mir_cleanup_ref, cleanup_at)

#undef ZEND_MIR_CFG_DELEGATE_AT

static bool zend_mir_cfg_view_root_at(const void *context, uint32_t index, uint32_t *out)
{
	const zend_mir_cfg *cfg = (const zend_mir_cfg *) context;

	return cfg->source->root_at(cfg->source->context, index, out);
}

static uint32_t zend_mir_cfg_view_block_count(const void *context)
{
	return ((const zend_mir_cfg *) context)->block_count;
}

static bool zend_mir_cfg_view_block_at(
		const void *context, uint32_t index, zend_mir_block_record *out)
{
	const zend_mir_cfg *cfg = (const zend_mir_cfg *) context;

	if (out == NULL || index >= cfg->block_count) {
		return false;
	}
	*out = cfg->blocks[index];
	return true;
}

static uint32_t zend_mir_cfg_view_instruction_count(const void *context)
{
	return ((const zend_mir_cfg *) context)->instruction_count;
}

static bool zend_mir_cfg_view_instruction_at(
		const void *context, uint32_t index, zend_mir_instruction_record *out)
{
	const zend_mir_cfg *cfg = (const zend_mir_cfg *) context;

	if (out == NULL || index >= cfg->instruction_count) {
		return false;
	}
	*out = cfg->instructions[index];
	return true;
}

static uint32_t zend_mir_cfg_view_operand_count(
		const void *context, zend_mir_instruction_id instruction_id)
{
	const zend_mir_cfg *cfg = (const zend_mir_cfg *) context;
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < cfg->operand_count; i++) {
		if (cfg->operands[i].instruction_id == instruction_id) {
			count++;
		}
	}
	return count;
}

static bool zend_mir_cfg_view_operand_at(const void *context,
		zend_mir_instruction_id instruction_id, uint32_t index,
		zend_mir_value_id *out)
{
	return zend_mir_cfg_phi_value_at(
		(const zend_mir_cfg *) context, instruction_id, index, out);
}

static uint32_t zend_mir_cfg_view_successor_count(
		const void *context, zend_mir_block_id block_id)
{
	return zend_mir_cfg_successor_count_internal(
		(const zend_mir_cfg *) context, block_id);
}

static uint32_t zend_mir_cfg_view_predecessor_count(
		const void *context, zend_mir_block_id block_id)
{
	return zend_mir_cfg_predecessor_count_internal(
		(const zend_mir_cfg *) context, block_id);
}

static bool zend_mir_cfg_view_edge_at(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id, uint32_t slot, bool successor,
		zend_mir_block_id *out)
{
	uint32_t i;

	if (out == NULL) {
		return false;
	}
	for (i = 0; i < cfg->edge_count; i++) {
		if ((successor ? cfg->edges[i].from : cfg->edges[i].to) == block_id
				&& (successor ? cfg->edges[i].successor_slot
					: cfg->edges[i].predecessor_slot) == slot) {
			*out = successor ? cfg->edges[i].to : cfg->edges[i].from;
			return true;
		}
	}
	return false;
}

static bool zend_mir_cfg_view_successor_at(const void *context,
		zend_mir_block_id block_id, uint32_t slot, zend_mir_block_id *out)
{
	return zend_mir_cfg_view_edge_at(
		(const zend_mir_cfg *) context, block_id, slot, true, out);
}

static bool zend_mir_cfg_view_predecessor_at(const void *context,
		zend_mir_block_id block_id, uint32_t slot, zend_mir_block_id *out)
{
	return zend_mir_cfg_view_edge_at(
		(const zend_mir_cfg *) context, block_id, slot, false, out);
}

static void zend_mir_cfg_initialize_view(zend_mir_cfg *cfg)
{
	memset(&cfg->view, 0, sizeof(cfg->view));
	cfg->view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	cfg->view.context = cfg;
	cfg->view.module_id = zend_mir_cfg_view_module_id;
	cfg->view.function_count = zend_mir_cfg_view_function_count;
	cfg->view.function_at = zend_mir_cfg_view_function_at;
	cfg->view.block_count = zend_mir_cfg_view_block_count;
	cfg->view.block_at = zend_mir_cfg_view_block_at;
	cfg->view.instruction_count = zend_mir_cfg_view_instruction_count;
	cfg->view.instruction_at = zend_mir_cfg_view_instruction_at;
	cfg->view.value_count = zend_mir_cfg_view_value_count;
	cfg->view.value_at = zend_mir_cfg_view_value_at;
	cfg->view.constant_count = zend_mir_cfg_view_constant_count;
	cfg->view.constant_at = zend_mir_cfg_view_constant_at;
	cfg->view.frame_state_count = zend_mir_cfg_view_frame_state_count;
	cfg->view.frame_state_at = zend_mir_cfg_view_frame_state_at;
	cfg->view.source_position_count = zend_mir_cfg_view_source_position_count;
	cfg->view.source_position_at = zend_mir_cfg_view_source_position_at;
	cfg->view.frame_slot_count = zend_mir_cfg_view_frame_slot_count;
	cfg->view.frame_slot_at = zend_mir_cfg_view_frame_slot_at;
	cfg->view.root_count = zend_mir_cfg_view_root_count;
	cfg->view.root_at = zend_mir_cfg_view_root_at;
	cfg->view.cleanup_count = zend_mir_cfg_view_cleanup_count;
	cfg->view.cleanup_at = zend_mir_cfg_view_cleanup_at;
	cfg->view.instruction_operand_count = zend_mir_cfg_view_operand_count;
	cfg->view.instruction_operand_at = zend_mir_cfg_view_operand_at;
	cfg->view.successor_count = zend_mir_cfg_view_successor_count;
	cfg->view.successor_at = zend_mir_cfg_view_successor_at;
	cfg->view.predecessor_count = zend_mir_cfg_view_predecessor_count;
	cfg->view.predecessor_at = zend_mir_cfg_view_predecessor_at;
}

static bool zend_mir_cfg_source_callbacks_valid(const zend_mir_view *view)
{
	return view != NULL && view->module_id != NULL
		&& view->function_count != NULL && view->function_at != NULL
		&& view->block_count != NULL && view->block_at != NULL
		&& view->instruction_count != NULL && view->instruction_at != NULL
		&& view->value_count != NULL && view->value_at != NULL
		&& view->constant_count != NULL && view->constant_at != NULL
		&& view->frame_state_count != NULL && view->frame_state_at != NULL
		&& view->source_position_count != NULL && view->source_position_at != NULL
		&& view->frame_slot_count != NULL && view->frame_slot_at != NULL
		&& view->root_count != NULL && view->root_at != NULL
		&& view->cleanup_count != NULL && view->cleanup_at != NULL
		&& view->instruction_operand_count != NULL
		&& view->instruction_operand_at != NULL
		&& view->successor_count != NULL && view->successor_at != NULL
		&& view->predecessor_count != NULL && view->predecessor_at != NULL;
}

static zend_mir_cfg_status zend_mir_cfg_snapshot_operands(zend_mir_cfg *cfg)
{
	zend_mir_cfg_status status;
	uint32_t total = 0;
	uint32_t i;
	uint32_t cursor = 0;

	for (i = 0; i < cfg->instruction_count; i++) {
		uint32_t count = cfg->source->instruction_operand_count(
			cfg->source->context, cfg->instructions[i].id);
		if (count > UINT32_MAX - total) {
			return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		}
		total += count;
	}
	if (total == 0) {
		return ZEND_MIR_CFG_STATUS_OK;
	}
	cfg->operands = (zend_mir_cfg_operand *) zend_mir_cfg_allocate(cfg, total,
		sizeof(zend_mir_cfg_operand), _Alignof(zend_mir_cfg_operand), &status);
	if (cfg->operands == NULL) {
		return status;
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		uint32_t j;
		uint32_t count = cfg->source->instruction_operand_count(
			cfg->source->context, cfg->instructions[i].id);
		for (j = 0; j < count; j++) {
			if (cursor >= total) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			cfg->operands[cursor].instruction_id = cfg->instructions[i].id;
			if (!cfg->source->instruction_operand_at(cfg->source->context,
					cfg->instructions[i].id, j, &cfg->operands[cursor].value_id)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			cursor++;
		}
	}
	if (cursor != total) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	cfg->operand_count = total;
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_cfg_snapshot_edges(zend_mir_cfg *cfg)
{
	zend_mir_cfg_status status;
	uint32_t total = 0;
	uint32_t i;
	uint32_t cursor = 0;

	for (i = 0; i < cfg->block_count; i++) {
		uint32_t count = cfg->source->successor_count(
			cfg->source->context, cfg->blocks[i].id);
		if (count > UINT32_MAX - total) {
			return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		}
		total += count;
	}
	if (total > INT_MAX) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	if (total == 0) {
		return ZEND_MIR_CFG_STATUS_OK;
	}
	cfg->edges = (zend_mir_cfg_edge *) zend_mir_cfg_allocate(cfg, total,
		sizeof(zend_mir_cfg_edge), _Alignof(zend_mir_cfg_edge), &status);
	if (cfg->edges == NULL) {
		return status;
	}
	for (i = 0; i < cfg->block_count; i++) {
		uint32_t j;
		uint32_t count = cfg->source->successor_count(
			cfg->source->context, cfg->blocks[i].id);
		for (j = 0; j < count; j++) {
			zend_mir_block_id to;
			uint32_t predecessor_count;
			uint32_t predecessor_slot;
			bool found = false;
			if (cursor >= total) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			if (!cfg->source->successor_at(cfg->source->context,
					cfg->blocks[i].id, j, &to)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			predecessor_count = cfg->source->predecessor_count(
				cfg->source->context, to);
			for (predecessor_slot = 0; predecessor_slot < predecessor_count;
					predecessor_slot++) {
				zend_mir_block_id predecessor;
				if (!cfg->source->predecessor_at(cfg->source->context, to,
						predecessor_slot, &predecessor)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (predecessor == cfg->blocks[i].id) {
					if (found) {
						return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
					}
					found = true;
					cfg->edges[cursor].predecessor_slot = predecessor_slot;
				}
			}
			if (!found) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			cfg->edges[cursor].from = cfg->blocks[i].id;
			cfg->edges[cursor].to = to;
			cfg->edges[cursor].successor_slot = j;
			cursor++;
		}
	}
	if (cursor != total) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	cfg->edge_count = total;
	/* Reject one-sided predecessor entries instead of normalizing them away. */
	for (i = 0; i < cfg->block_count; i++) {
		uint32_t predecessor_count = cfg->source->predecessor_count(
			cfg->source->context, cfg->blocks[i].id);
		uint32_t predecessor_slot;
		for (predecessor_slot = 0; predecessor_slot < predecessor_count;
				predecessor_slot++) {
			zend_mir_block_id from;
			uint32_t successor_count;
			uint32_t successor_slot;
			uint32_t matches = 0;
			if (!cfg->source->predecessor_at(cfg->source->context,
					cfg->blocks[i].id, predecessor_slot, &from)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			successor_count = cfg->source->successor_count(
				cfg->source->context, from);
			for (successor_slot = 0; successor_slot < successor_count;
					successor_slot++) {
				zend_mir_block_id to;
				if (!cfg->source->successor_at(cfg->source->context, from,
						successor_slot, &to)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (to == cfg->blocks[i].id) {
					matches++;
				}
			}
			if (matches != 1) {
				return matches == 0 ? ZEND_MIR_CFG_STATUS_INVALID_CFG
					: ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
			}
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_create(zend_mir_cfg **out,
		const zend_mir_view *source, zend_mir_function_id function_id,
		zend_mir_allocator allocator, zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_cfg *cfg;
	zend_mir_cfg_status status;
	zend_mir_function_record function;
	uint32_t i;

	if (out == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	*out = NULL;
	if (allocator.allocate == NULL || allocator.reset == NULL
			|| !zend_mir_cfg_source_callbacks_valid(source)
			|| !zend_mir_id_is_valid(function_id)) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	if (!zend_mir_contract_is_compatible(source->contract_version)) {
		return ZEND_MIR_CFG_STATUS_INCOMPATIBLE_CONTRACT;
	}
	cfg = (zend_mir_cfg *) allocator.allocate(
		allocator.context, sizeof(zend_mir_cfg), _Alignof(zend_mir_cfg));
	if (cfg == NULL) {
		return ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED;
	}
	memset(cfg, 0, sizeof(*cfg));
	cfg->allocator = allocator;
	cfg->diagnostics = diagnostics;
	cfg->source = source;
	cfg->function_id = function_id;
	zend_mir_cfg_initialize_view(cfg);
	if (!zend_mir_cfg_find_function_record(cfg, function_id, &function)) {
		status = ZEND_MIR_CFG_STATUS_NOT_FOUND;
		goto fail;
	}
	cfg->block_count = source->block_count(source->context);
	if (cfg->block_count > INT_MAX) {
		status = ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		goto fail;
	}
	if (cfg->block_count != 0) {
		cfg->blocks = (zend_mir_block_record *) zend_mir_cfg_allocate(cfg,
			cfg->block_count, sizeof(zend_mir_block_record),
			_Alignof(zend_mir_block_record), &status);
		if (cfg->blocks == NULL) {
			goto fail;
		}
	}
	for (i = 0; i < cfg->block_count; i++) {
		if (!source->block_at(source->context, i, &cfg->blocks[i])) {
			status = ZEND_MIR_CFG_STATUS_INVALID_CFG;
			goto fail;
		}
	}
	cfg->instruction_count = source->instruction_count(source->context);
	if (cfg->instruction_count > INT_MAX) {
		status = ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		goto fail;
	}
	if (cfg->instruction_count != 0) {
		cfg->instructions = (zend_mir_instruction_record *) zend_mir_cfg_allocate(cfg,
			cfg->instruction_count, sizeof(zend_mir_instruction_record),
			_Alignof(zend_mir_instruction_record), &status);
		if (cfg->instructions == NULL) {
			goto fail;
		}
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		if (!source->instruction_at(source->context, i, &cfg->instructions[i])) {
			status = ZEND_MIR_CFG_STATUS_INVALID_CFG;
			goto fail;
		}
	}
	status = zend_mir_cfg_snapshot_operands(cfg);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	status = zend_mir_cfg_snapshot_edges(cfg);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	status = zend_mir_cfg_validate(cfg);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	*out = cfg;
	return ZEND_MIR_CFG_STATUS_OK;

fail:
	zend_mir_cfg_emit(cfg, status, ZEND_MIR_ID_INVALID, ZEND_MIR_ID_INVALID,
		"cannot construct CFG snapshot");
	allocator.reset(allocator.context);
	return status;
}

void zend_mir_cfg_destroy(zend_mir_cfg *cfg)
{
	zend_mir_reset_fn reset;
	void *context;

	if (cfg == NULL) {
		return;
	}
	reset = cfg->allocator.reset;
	context = cfg->allocator.context;
	if (reset != NULL) {
		reset(context);
	}
}

const zend_mir_view *zend_mir_cfg_view(const zend_mir_cfg *cfg)
{
	return cfg == NULL ? NULL : &cfg->view;
}

zend_mir_function_id zend_mir_cfg_function_id(const zend_mir_cfg *cfg)
{
	return cfg == NULL ? ZEND_MIR_ID_INVALID : cfg->function_id;
}

const char *zend_mir_cfg_status_name(zend_mir_cfg_status status)
{
	switch (status) {
		case ZEND_MIR_CFG_STATUS_OK: return "ok";
		case ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT: return "invalid_argument";
		case ZEND_MIR_CFG_STATUS_INCOMPATIBLE_CONTRACT: return "incompatible_contract";
		case ZEND_MIR_CFG_STATUS_NOT_FOUND: return "not_found";
		case ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE: return "duplicate_edge";
		case ZEND_MIR_CFG_STATUS_INVALID_CFG: return "invalid_cfg";
		case ZEND_MIR_CFG_STATUS_INVALID_PHI: return "invalid_phi";
		case ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED: return "allocation_failed";
		case ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED: return "capacity_exceeded";
	}
	return "invalid_status";
}

zend_mir_cfg_status zend_mir_cfg_validate(const zend_mir_cfg *cfg)
{
	zend_mir_function_record function;
	uint32_t i;

	if (cfg == NULL || !zend_mir_cfg_find_function_record(cfg, cfg->function_id, &function)
			|| !zend_mir_cfg_block_is_selected(cfg, function.entry_block_id)) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	for (i = 0; i < cfg->block_count; i++) {
		uint32_t slot;
		uint32_t successors;
		uint32_t predecessors;
		uint32_t j;
		if (!zend_mir_id_is_valid(cfg->blocks[i].id)) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		for (j = i + 1; j < cfg->block_count; j++) {
			if (cfg->blocks[i].id == cfg->blocks[j].id) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
		successors = zend_mir_cfg_successor_count_internal(cfg, cfg->blocks[i].id);
		predecessors = zend_mir_cfg_predecessor_count_internal(cfg, cfg->blocks[i].id);
		for (slot = 0; slot < successors; slot++) {
			if (!zend_mir_cfg_edge_slot_exists(cfg, cfg->blocks[i].id, slot, true)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
		for (slot = 0; slot < predecessors; slot++) {
			if (!zend_mir_cfg_edge_slot_exists(cfg, cfg->blocks[i].id, slot, false)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
	}
	for (i = 0; i < cfg->edge_count; i++) {
		int from = zend_mir_cfg_find_block(cfg, cfg->edges[i].from);
		int to = zend_mir_cfg_find_block(cfg, cfg->edges[i].to);
		uint32_t j;
		if (from < 0 || to < 0
				|| cfg->blocks[from].function_id != cfg->blocks[to].function_id) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		for (j = i + 1; j < cfg->edge_count; j++) {
			if (cfg->edges[i].from == cfg->edges[j].from
					&& cfg->edges[i].to == cfg->edges[j].to) {
				return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
			}
		}
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		uint32_t operands;
		uint32_t j;
		if (!zend_mir_id_is_valid(cfg->instructions[i].id)
				|| zend_mir_cfg_find_block(cfg, cfg->instructions[i].block_id) < 0) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		for (j = i + 1; j < cfg->instruction_count; j++) {
			if (cfg->instructions[i].id == cfg->instructions[j].id) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
		operands = cfg->view.instruction_operand_count(
			cfg->view.context, cfg->instructions[i].id);
		if (cfg->instructions[i].opcode == ZEND_MIR_OPCODE_PHI
				&& operands != zend_mir_cfg_predecessor_count_internal(
					cfg, cfg->instructions[i].block_id)) {
			return ZEND_MIR_CFG_STATUS_INVALID_PHI;
		}
	}
	for (i = 0; i < cfg->block_count; i++) {
		zend_mir_instruction_record terminator;
		uint32_t instruction_count;
		uint32_t expected;
		uint32_t j;
		bool saw_non_phi = false;
		if (cfg->blocks[i].function_id != cfg->function_id) {
			continue;
		}
		if (!zend_mir_cfg_block_terminator(cfg, cfg->blocks[i].id,
				&terminator, &instruction_count) || instruction_count == 0) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		expected = zend_mir_cfg_expected_successors(terminator.opcode);
		if (expected == UINT32_MAX || expected != zend_mir_cfg_successor_count_internal(
				cfg, cfg->blocks[i].id)) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		for (j = 0; j < cfg->instruction_count; j++) {
			if (cfg->instructions[j].block_id != cfg->blocks[i].id) {
				continue;
			}
			if (zend_mir_opcode_is_terminator(cfg->instructions[j].opcode)
					&& cfg->instructions[j].id != terminator.id) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			if (cfg->instructions[j].opcode == ZEND_MIR_OPCODE_PHI) {
				if (saw_non_phi) {
					return ZEND_MIR_CFG_STATUS_INVALID_PHI;
				}
			} else {
				saw_non_phi = true;
			}
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_cfg_validate_incoming(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id, const zend_mir_cfg_phi_incoming *incoming,
		uint32_t incoming_count)
{
	uint32_t phi_count = zend_mir_cfg_phi_count_internal(cfg, block_id);
	uint32_t i;

	if (incoming_count != phi_count || (incoming_count != 0 && incoming == NULL)) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	for (i = 0; i < incoming_count; i++) {
		int instruction = zend_mir_cfg_find_instruction(
			cfg, incoming[i].phi_instruction_id);
		uint32_t j;
		if (instruction < 0
				|| cfg->instructions[instruction].block_id != block_id
				|| cfg->instructions[instruction].opcode != ZEND_MIR_OPCODE_PHI
				|| !zend_mir_cfg_value_exists(cfg, incoming[i].value_id)) {
			return ZEND_MIR_CFG_STATUS_INVALID_PHI;
		}
		for (j = i + 1; j < incoming_count; j++) {
			if (incoming[i].phi_instruction_id == incoming[j].phi_instruction_id) {
				return ZEND_MIR_CFG_STATUS_INVALID_PHI;
			}
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_cfg_copy_edges(zend_mir_cfg *cfg,
		uint32_t count, zend_mir_cfg_edge **out)
{
	zend_mir_cfg_status status;
	if (count == 0) {
		*out = NULL;
		return ZEND_MIR_CFG_STATUS_OK;
	}
	*out = (zend_mir_cfg_edge *) zend_mir_cfg_allocate(cfg, count,
		sizeof(zend_mir_cfg_edge), _Alignof(zend_mir_cfg_edge), &status);
	if (*out == NULL) {
		return status;
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_cfg_copy_operands(zend_mir_cfg *cfg,
		uint32_t count, zend_mir_cfg_operand **out)
{
	zend_mir_cfg_status status;
	if (count == 0) {
		*out = NULL;
		return ZEND_MIR_CFG_STATUS_OK;
	}
	*out = (zend_mir_cfg_operand *) zend_mir_cfg_allocate(cfg, count,
		sizeof(zend_mir_cfg_operand), _Alignof(zend_mir_cfg_operand), &status);
	if (*out == NULL) {
		return status;
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_add_edge(zend_mir_cfg *cfg,
		zend_mir_block_id from, zend_mir_block_id to,
		const zend_mir_cfg_phi_incoming *incoming, uint32_t incoming_count)
{
	zend_mir_cfg_edge *edges;
	zend_mir_cfg_operand *operands;
	zend_mir_cfg_status status;
	uint32_t phi_count;
	uint32_t i;

	if (cfg == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	if (!zend_mir_cfg_block_is_selected(cfg, from)
			|| !zend_mir_cfg_block_is_selected(cfg, to)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	if (zend_mir_cfg_find_edge(cfg, from, to) >= 0) {
		return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
	}
	status = zend_mir_cfg_validate_incoming(cfg, to, incoming, incoming_count);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	if (cfg->edge_count == UINT32_MAX || cfg->operand_count > UINT32_MAX - incoming_count) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	status = zend_mir_cfg_copy_edges(cfg, cfg->edge_count + 1, &edges);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_copy_operands(cfg, cfg->operand_count + incoming_count, &operands);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	if (cfg->edge_count != 0) {
		memcpy(edges, cfg->edges, cfg->edge_count * sizeof(*edges));
	}
	if (cfg->operand_count != 0) {
		if (cfg->operands == NULL || operands == NULL) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		memcpy(operands, cfg->operands, cfg->operand_count * sizeof(*operands));
	}
	edges[cfg->edge_count].from = from;
	edges[cfg->edge_count].to = to;
	edges[cfg->edge_count].successor_slot = zend_mir_cfg_successor_count_internal(cfg, from);
	edges[cfg->edge_count].predecessor_slot = zend_mir_cfg_predecessor_count_internal(cfg, to);
	phi_count = incoming_count;
	for (i = 0; i < phi_count; i++) {
		operands[cfg->operand_count + i].instruction_id = incoming[i].phi_instruction_id;
		operands[cfg->operand_count + i].value_id = incoming[i].value_id;
	}
	cfg->edges = edges;
	cfg->edge_count++;
	cfg->operands = operands;
	cfg->operand_count += incoming_count;
	return ZEND_MIR_CFG_STATUS_OK;
}

static bool zend_mir_cfg_operand_is_removed(const zend_mir_cfg *cfg,
		const zend_mir_cfg_operand *operand, zend_mir_block_id block_id,
		uint32_t predecessor_slot)
{
	int instruction = zend_mir_cfg_find_instruction(cfg, operand->instruction_id);
	uint32_t slot = 0;
	uint32_t i;

	if (instruction < 0 || cfg->instructions[instruction].block_id != block_id
			|| cfg->instructions[instruction].opcode != ZEND_MIR_OPCODE_PHI) {
		return false;
	}
	for (i = 0; i < cfg->operand_count; i++) {
		if (cfg->operands[i].instruction_id == operand->instruction_id) {
			if (&cfg->operands[i] == operand) {
				return slot == predecessor_slot;
			}
			slot++;
		}
	}
	return false;
}

zend_mir_cfg_status zend_mir_cfg_remove_edge(zend_mir_cfg *cfg,
		zend_mir_block_id from, zend_mir_block_id to)
{
	int edge_index;
	uint32_t phi_count;
	uint32_t old_slot;
	uint32_t old_successor_slot;
	zend_mir_cfg_edge *edges;
	zend_mir_cfg_operand *operands;
	zend_mir_cfg_status status;
	uint32_t i;
	uint32_t edge_cursor = 0;
	uint32_t operand_cursor = 0;

	if (cfg == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	edge_index = zend_mir_cfg_find_edge(cfg, from, to);
	if (edge_index < 0 || !zend_mir_cfg_block_is_selected(cfg, from)
			|| !zend_mir_cfg_block_is_selected(cfg, to)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	phi_count = zend_mir_cfg_phi_count_internal(cfg, to);
	if (cfg->operand_count < phi_count) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	old_slot = cfg->edges[edge_index].predecessor_slot;
	old_successor_slot = cfg->edges[edge_index].successor_slot;
	for (i = 0; i < cfg->instruction_count; i++) {
		zend_mir_value_id ignored;
		if (cfg->instructions[i].block_id == to
				&& cfg->instructions[i].opcode == ZEND_MIR_OPCODE_PHI
				&& !zend_mir_cfg_phi_value_at(cfg, cfg->instructions[i].id,
					old_slot, &ignored)) {
			return ZEND_MIR_CFG_STATUS_INVALID_PHI;
		}
	}
	status = zend_mir_cfg_copy_edges(cfg, cfg->edge_count - 1, &edges);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_copy_operands(cfg, cfg->operand_count - phi_count, &operands);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	for (i = 0; i < cfg->edge_count; i++) {
		zend_mir_cfg_edge edge;
		if ((int) i == edge_index) {
			continue;
		}
		edge = cfg->edges[i];
		if (edge.from == from && edge.successor_slot > old_successor_slot) {
			edge.successor_slot--;
		}
		if (edge.to == to && edge.predecessor_slot > old_slot) {
			edge.predecessor_slot--;
		}
		if (edge_cursor >= cfg->edge_count - 1 || edges == NULL) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		edges[edge_cursor++] = edge;
	}
	for (i = 0; i < cfg->operand_count; i++) {
		if (!zend_mir_cfg_operand_is_removed(cfg, &cfg->operands[i], to, old_slot)) {
			if (operand_cursor >= cfg->operand_count - phi_count
					|| operands == NULL) {
				return ZEND_MIR_CFG_STATUS_INVALID_PHI;
			}
			operands[operand_cursor++] = cfg->operands[i];
		}
	}
	if (edge_cursor != cfg->edge_count - 1
			|| operand_cursor != cfg->operand_count - phi_count) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	cfg->edges = edges;
	cfg->edge_count--;
	cfg->operands = operands;
	cfg->operand_count -= phi_count;
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_retarget_edge(zend_mir_cfg *cfg,
		zend_mir_block_id from, zend_mir_block_id old_to, zend_mir_block_id new_to,
		const zend_mir_cfg_phi_incoming *incoming, uint32_t incoming_count)
{
	int edge_index;
	uint32_t old_phi_count;
	uint32_t new_phi_count;
	uint32_t old_slot;
	uint32_t new_slot;
	zend_mir_cfg_edge *edges;
	zend_mir_cfg_operand *operands;
	zend_mir_cfg_status status;
	uint32_t i;
	uint32_t cursor = 0;

	if (cfg == NULL || old_to == new_to) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	edge_index = zend_mir_cfg_find_edge(cfg, from, old_to);
	if (edge_index < 0 || !zend_mir_cfg_block_is_selected(cfg, from)
			|| !zend_mir_cfg_block_is_selected(cfg, old_to)
			|| !zend_mir_cfg_block_is_selected(cfg, new_to)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	if (zend_mir_cfg_find_edge(cfg, from, new_to) >= 0) {
		return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
	}
	status = zend_mir_cfg_validate_incoming(cfg, new_to, incoming, incoming_count);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	old_phi_count = zend_mir_cfg_phi_count_internal(cfg, old_to);
	new_phi_count = zend_mir_cfg_phi_count_internal(cfg, new_to);
	if (cfg->operand_count < old_phi_count) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	if (cfg->operand_count - old_phi_count > UINT32_MAX - new_phi_count) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	old_slot = cfg->edges[edge_index].predecessor_slot;
	new_slot = zend_mir_cfg_predecessor_count_internal(cfg, new_to);
	status = zend_mir_cfg_copy_edges(cfg, cfg->edge_count, &edges);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_copy_operands(cfg,
		cfg->operand_count - old_phi_count + new_phi_count, &operands);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	if (cfg->edge_count == 0 || cfg->edges == NULL || edges == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	memcpy(edges, cfg->edges, cfg->edge_count * sizeof(*edges));
	edges[edge_index].to = new_to;
	edges[edge_index].predecessor_slot = new_slot;
	for (i = 0; i < cfg->edge_count; i++) {
		if ((int) i != edge_index && edges[i].to == old_to
				&& edges[i].predecessor_slot > old_slot) {
			edges[i].predecessor_slot--;
		}
	}
	for (i = 0; i < cfg->operand_count; i++) {
		if (!zend_mir_cfg_operand_is_removed(cfg, &cfg->operands[i], old_to, old_slot)) {
			operands[cursor++] = cfg->operands[i];
		}
	}
	if (cursor != cfg->operand_count - old_phi_count) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	for (i = 0; i < incoming_count; i++) {
		operands[cursor].instruction_id = incoming[i].phi_instruction_id;
		operands[cursor].value_id = incoming[i].value_id;
		cursor++;
	}
	cfg->edges = edges;
	cfg->operands = operands;
	cfg->operand_count = cursor;
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_next_block_id(const zend_mir_cfg *cfg,
		zend_mir_block_id *out)
{
	zend_mir_block_id maximum = 0;
	uint32_t i;

	if (cfg == NULL || out == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	for (i = 0; i < cfg->block_count; i++) {
		if (cfg->blocks[i].id > maximum) {
			maximum = cfg->blocks[i].id;
		}
	}
	if (maximum >= ZEND_MIR_ID_MAX) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	*out = cfg->block_count == 0 ? 0 : maximum + 1;
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_next_instruction_id(const zend_mir_cfg *cfg,
		zend_mir_instruction_id *out)
{
	zend_mir_instruction_id maximum = 0;
	uint32_t i;

	if (cfg == NULL || out == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		if (cfg->instructions[i].id > maximum) {
			maximum = cfg->instructions[i].id;
		}
	}
	if (maximum >= ZEND_MIR_ID_MAX) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	*out = cfg->instruction_count == 0 ? 0 : maximum + 1;
	return ZEND_MIR_CFG_STATUS_OK;
}

static void zend_mir_cfg_make_branch(zend_mir_instruction_record *instruction,
		zend_mir_instruction_id id, zend_mir_block_id block_id)
{
	memset(instruction, 0, sizeof(*instruction));
	instruction->id = id;
	instruction->block_id = block_id;
	instruction->opcode = ZEND_MIR_OPCODE_BRANCH;
	instruction->representation = ZEND_MIR_REPRESENTATION_CONTROL;
	instruction->result_id = ZEND_MIR_ID_INVALID;
	instruction->frame_state_id = ZEND_MIR_ID_INVALID;
	instruction->source_position_id = ZEND_MIR_ID_INVALID;
}

static zend_mir_cfg_status zend_mir_cfg_allocate_split_arrays(zend_mir_cfg *cfg,
		zend_mir_block_record **blocks, zend_mir_instruction_record **instructions,
		zend_mir_cfg_edge **edges)
{
	zend_mir_cfg_status status;
	if (cfg->block_count == UINT32_MAX || cfg->instruction_count == UINT32_MAX
			|| cfg->edge_count == UINT32_MAX) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	*blocks = (zend_mir_block_record *) zend_mir_cfg_allocate(cfg,
		cfg->block_count + 1, sizeof(zend_mir_block_record),
		_Alignof(zend_mir_block_record), &status);
	if (*blocks == NULL) {
		return status;
	}
	*instructions = (zend_mir_instruction_record *) zend_mir_cfg_allocate(cfg,
		cfg->instruction_count + 1, sizeof(zend_mir_instruction_record),
		_Alignof(zend_mir_instruction_record), &status);
	if (*instructions == NULL) {
		return status;
	}
	*edges = (zend_mir_cfg_edge *) zend_mir_cfg_allocate(cfg,
		cfg->edge_count + 1, sizeof(zend_mir_cfg_edge),
		_Alignof(zend_mir_cfg_edge), &status);
	if (*edges == NULL) {
		return status;
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_split_edge(zend_mir_cfg *cfg,
		zend_mir_block_id from, zend_mir_block_id to,
		zend_mir_block_id *new_block_id)
{
	int edge_index;
	int from_index;
	zend_mir_block_id block_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_block_record *blocks;
	zend_mir_instruction_record *instructions;
	zend_mir_cfg_edge *edges;
	zend_mir_cfg_status status;
	zend_mir_cfg_edge old_edge;

	if (cfg == NULL || new_block_id == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	edge_index = zend_mir_cfg_find_edge(cfg, from, to);
	from_index = zend_mir_cfg_find_block(cfg, from);
	if (edge_index < 0 || from_index < 0
			|| !zend_mir_cfg_block_is_selected(cfg, from)
			|| !zend_mir_cfg_block_is_selected(cfg, to)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	status = zend_mir_cfg_validate(cfg);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_next_block_id(cfg, &block_id);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_next_instruction_id(cfg, &instruction_id);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_allocate_split_arrays(cfg, &blocks, &instructions, &edges);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	memcpy(blocks, cfg->blocks, cfg->block_count * sizeof(*blocks));
	memcpy(instructions, cfg->instructions,
		cfg->instruction_count * sizeof(*instructions));
	if (cfg->edge_count != 0) {
		memcpy(edges, cfg->edges, cfg->edge_count * sizeof(*edges));
	}
	blocks[cfg->block_count].id = block_id;
	blocks[cfg->block_count].function_id = cfg->blocks[from_index].function_id;
	zend_mir_cfg_make_branch(&instructions[cfg->instruction_count],
		instruction_id, block_id);
	old_edge = edges[edge_index];
	edges[edge_index].to = block_id;
	edges[edge_index].predecessor_slot = 0;
	edges[cfg->edge_count].from = block_id;
	edges[cfg->edge_count].to = to;
	edges[cfg->edge_count].successor_slot = 0;
	edges[cfg->edge_count].predecessor_slot = old_edge.predecessor_slot;
	cfg->blocks = blocks;
	cfg->block_count++;
	cfg->instructions = instructions;
	cfg->instruction_count++;
	cfg->edges = edges;
	cfg->edge_count++;
	*new_block_id = block_id;
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_split_block(zend_mir_cfg *cfg,
		zend_mir_block_id block_id, zend_mir_instruction_id instruction_id,
		zend_mir_block_id *new_block_id)
{
	int block_index;
	int instruction_index;
	zend_mir_block_id split_id;
	zend_mir_instruction_id branch_id;
	zend_mir_block_record *blocks;
	zend_mir_instruction_record *instructions;
	zend_mir_cfg_edge *edges;
	zend_mir_cfg_status status;
	uint32_t i;
	uint32_t moved = 0;

	if (cfg == NULL || new_block_id == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	block_index = zend_mir_cfg_find_block(cfg, block_id);
	instruction_index = zend_mir_cfg_find_instruction(cfg, instruction_id);
	if (block_index < 0 || instruction_index < 0
			|| !zend_mir_cfg_block_is_selected(cfg, block_id)
			|| cfg->instructions[instruction_index].block_id != block_id) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	if (cfg->instructions[instruction_index].opcode == ZEND_MIR_OPCODE_PHI) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	status = zend_mir_cfg_validate(cfg);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	for (i = (uint32_t) instruction_index; i < cfg->instruction_count; i++) {
		if (cfg->instructions[i].block_id == block_id) {
			if (cfg->instructions[i].opcode == ZEND_MIR_OPCODE_PHI) {
				return ZEND_MIR_CFG_STATUS_INVALID_PHI;
			}
			moved++;
		}
	}
	if (moved == 0) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	status = zend_mir_cfg_next_block_id(cfg, &split_id);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_next_instruction_id(cfg, &branch_id);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	status = zend_mir_cfg_allocate_split_arrays(cfg, &blocks, &instructions, &edges);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		return status;
	}
	memcpy(blocks, cfg->blocks, cfg->block_count * sizeof(*blocks));
	if (instruction_index != 0) {
		memcpy(instructions, cfg->instructions,
			(size_t) instruction_index * sizeof(*instructions));
	}
	zend_mir_cfg_make_branch(&instructions[instruction_index], branch_id, block_id);
	memcpy(&instructions[instruction_index + 1],
		&cfg->instructions[instruction_index],
		(size_t) (cfg->instruction_count - (uint32_t) instruction_index)
			* sizeof(*instructions));
	if (cfg->edge_count != 0) {
		memcpy(edges, cfg->edges, cfg->edge_count * sizeof(*edges));
	}
	blocks[cfg->block_count].id = split_id;
	blocks[cfg->block_count].function_id = cfg->blocks[block_index].function_id;
	for (i = (uint32_t) instruction_index + 1; i <= cfg->instruction_count; i++) {
		if (instructions[i].block_id == block_id) {
			instructions[i].block_id = split_id;
		}
	}
	for (i = 0; i < cfg->edge_count; i++) {
		if (edges[i].from == block_id) {
			edges[i].from = split_id;
		}
	}
	edges[cfg->edge_count].from = block_id;
	edges[cfg->edge_count].to = split_id;
	edges[cfg->edge_count].successor_slot = 0;
	edges[cfg->edge_count].predecessor_slot = 0;
	cfg->blocks = blocks;
	cfg->block_count++;
	cfg->instructions = instructions;
	cfg->instruction_count++;
	cfg->edges = edges;
	cfg->edge_count++;
	*new_block_id = split_id;
	return ZEND_MIR_CFG_STATUS_OK;
}
