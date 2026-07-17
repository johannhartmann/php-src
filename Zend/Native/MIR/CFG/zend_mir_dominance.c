#include "zend_mir_dominance.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct _zend_mir_dominance {
	zend_mir_allocator allocator;
	zend_mir_block_id *block_ids;
	zend_mir_block_id *immediate_dominators;
	uint64_t *sets;
	bool *reachable;
	uint32_t block_count;
	uint32_t word_count;
};

static bool zend_mir_dominance_size(size_t count, size_t width, size_t *out)
{
	if (out == NULL || (width != 0 && count > SIZE_MAX / width)) {
		return false;
	}
	*out = count * width;
	return true;
}

static void *zend_mir_dominance_allocate(zend_mir_allocator allocator,
		size_t count, size_t width, size_t alignment, zend_mir_cfg_status *status)
{
	size_t size;
	void *allocation;

	if (status == NULL) {
		return NULL;
	}
	if (count == 0) {
		*status = ZEND_MIR_CFG_STATUS_OK;
		return NULL;
	}
	if (!zend_mir_dominance_size(count, width, &size)) {
		*status = ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		return NULL;
	}
	allocation = allocator.allocate(allocator.context, size, alignment);
	*status = allocation == NULL ? ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED
		: ZEND_MIR_CFG_STATUS_OK;
	return allocation;
}

static int zend_mir_dominance_compare_ids(const void *left, const void *right)
{
	zend_mir_block_id a = *(const zend_mir_block_id *) left;
	zend_mir_block_id b = *(const zend_mir_block_id *) right;

	return a < b ? -1 : a != b;
}

static int zend_mir_dominance_find(
		const zend_mir_dominance *dominance, zend_mir_block_id block_id)
{
	uint32_t low = 0;
	uint32_t high;

	if (dominance == NULL) {
		return -1;
	}
	high = dominance->block_count;
	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		if (dominance->block_ids[middle] == block_id) {
			return (int) middle;
		}
		if (dominance->block_ids[middle] < block_id) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return -1;
}

static uint64_t *zend_mir_dominance_set(
		const zend_mir_dominance *dominance, uint32_t block_index)
{
	return dominance->sets + (size_t) block_index * dominance->word_count;
}

static bool zend_mir_dominance_set_contains(
		const zend_mir_dominance *dominance, uint32_t block_index, uint32_t member)
{
	const uint64_t *set = zend_mir_dominance_set(dominance, block_index);

	return (set[member / 64] & (UINT64_C(1) << (member % 64))) != 0;
}

static void zend_mir_dominance_set_member(
		const zend_mir_dominance *dominance, uint64_t *set, uint32_t member)
{
	(void) dominance;
	set[member / 64] |= UINT64_C(1) << (member % 64);
}

static bool zend_mir_dominance_find_function(const zend_mir_view *view,
		zend_mir_function_id function_id, zend_mir_function_record *out)
{
	uint32_t i;

	for (i = 0; i < view->function_count(view->context); i++) {
		if (!view->function_at(view->context, i, out)) {
			return false;
		}
		if (out->id == function_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_dominance_callbacks_valid(const zend_mir_view *view)
{
	return view != NULL
		&& view->function_count != NULL && view->function_at != NULL
		&& view->block_count != NULL && view->block_at != NULL
		&& view->successor_count != NULL && view->successor_at != NULL
		&& view->predecessor_count != NULL && view->predecessor_at != NULL;
}

static void zend_mir_dominance_emit(zend_mir_diagnostic_sink *sink,
		zend_mir_function_id function_id, zend_mir_diagnostic_code code,
		const char *message)
{
	zend_mir_diagnostic diagnostic;

	if (sink == NULL) {
		return;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = code;
	diagnostic.severity = code == ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED
		? ZEND_MIR_DIAGNOSTIC_FATAL : ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = function_id;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	if (message != NULL) {
		size_t length = strlen(message);
		if (length >= sizeof(diagnostic.message)) {
			length = sizeof(diagnostic.message) - 1;
		}
		memcpy(diagnostic.message, message, length);
		diagnostic.message[length] = '\0';
	}
	(void) zend_mir_diagnostic_sink_emit(sink, &diagnostic);
}

static zend_mir_cfg_status zend_mir_dominance_collect_blocks(
		zend_mir_dominance *dominance, const zend_mir_view *view,
		zend_mir_function_id function_id)
{
	uint32_t total = view->block_count(view->context);
	uint32_t count = 0;
	uint32_t i;
	zend_mir_block_record block;
	zend_mir_cfg_status status;

	for (i = 0; i < total; i++) {
		if (!view->block_at(view->context, i, &block)) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		if (block.function_id == function_id) {
			if (count == UINT32_MAX) {
				return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
			}
			count++;
		}
	}
	if (count == 0) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	if (count > INT_MAX) {
		return ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
	}
	dominance->block_ids = (zend_mir_block_id *) zend_mir_dominance_allocate(
		dominance->allocator, count, sizeof(zend_mir_block_id),
		_Alignof(zend_mir_block_id), &status);
	if (dominance->block_ids == NULL) {
		return status;
	}
	count = 0;
	for (i = 0; i < total; i++) {
		if (!view->block_at(view->context, i, &block)) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
		if (block.function_id == function_id) {
			dominance->block_ids[count++] = block.id;
		}
	}
	dominance->block_count = count;
	qsort(dominance->block_ids, count, sizeof(*dominance->block_ids),
		zend_mir_dominance_compare_ids);
	for (i = 0; i < count; i++) {
		if (!zend_mir_id_is_valid(dominance->block_ids[i])
				|| (i != 0 && dominance->block_ids[i - 1] == dominance->block_ids[i])) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_dominance_mark_reachable(
		zend_mir_dominance *dominance, const zend_mir_view *view,
		zend_mir_block_id entry_id, uint32_t *queue)
{
	int entry = zend_mir_dominance_find(dominance, entry_id);
	uint32_t head = 0;
	uint32_t tail = 0;

	if (entry < 0) {
		return ZEND_MIR_CFG_STATUS_INVALID_CFG;
	}
	dominance->reachable[entry] = true;
	queue[tail++] = (uint32_t) entry;
	while (head < tail) {
		uint32_t block_index = queue[head++];
		zend_mir_block_id block_id = dominance->block_ids[block_index];
		uint32_t count = view->successor_count(view->context, block_id);
		uint32_t slot;
		for (slot = 0; slot < count; slot++) {
			zend_mir_block_id successor_id;
			int successor;
			if (!view->successor_at(view->context, block_id, slot, &successor_id)) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			successor = zend_mir_dominance_find(dominance, successor_id);
			if (successor < 0) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			if (!dominance->reachable[successor]) {
				dominance->reachable[successor] = true;
				queue[tail++] = (uint32_t) successor;
			}
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_dominance_validate_edges(
		const zend_mir_dominance *dominance, const zend_mir_view *view)
{
	uint32_t block;

	for (block = 0; block < dominance->block_count; block++) {
		zend_mir_block_id block_id = dominance->block_ids[block];
		uint32_t successor_count = view->successor_count(view->context, block_id);
		uint32_t predecessor_count = view->predecessor_count(view->context, block_id);
		uint32_t slot;
		for (slot = 0; slot < successor_count; slot++) {
			zend_mir_block_id successor_id;
			uint32_t reverse_count;
			uint32_t reverse_slot;
			uint32_t matches = 0;
			if (!view->successor_at(view->context, block_id, slot, &successor_id)
					|| zend_mir_dominance_find(dominance, successor_id) < 0) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			for (reverse_slot = 0; reverse_slot < slot; reverse_slot++) {
				zend_mir_block_id earlier;
				if (!view->successor_at(
						view->context, block_id, reverse_slot, &earlier)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (earlier == successor_id) {
					return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
				}
			}
			reverse_count = view->predecessor_count(view->context, successor_id);
			for (reverse_slot = 0; reverse_slot < reverse_count; reverse_slot++) {
				zend_mir_block_id predecessor_id;
				if (!view->predecessor_at(view->context, successor_id,
						reverse_slot, &predecessor_id)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (predecessor_id == block_id) {
					matches++;
				}
			}
			if (matches != 1) {
				return matches > 1 ? ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE
					: ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
		for (slot = 0; slot < predecessor_count; slot++) {
			zend_mir_block_id predecessor_id;
			uint32_t reverse_count;
			uint32_t reverse_slot;
			uint32_t matches = 0;
			if (!view->predecessor_at(view->context, block_id, slot,
					&predecessor_id)
					|| zend_mir_dominance_find(dominance, predecessor_id) < 0) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			for (reverse_slot = 0; reverse_slot < slot; reverse_slot++) {
				zend_mir_block_id earlier;
				if (!view->predecessor_at(
						view->context, block_id, reverse_slot, &earlier)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (earlier == predecessor_id) {
					return ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE;
				}
			}
			reverse_count = view->successor_count(view->context, predecessor_id);
			for (reverse_slot = 0; reverse_slot < reverse_count; reverse_slot++) {
				zend_mir_block_id successor_id;
				if (!view->successor_at(view->context, predecessor_id,
						reverse_slot, &successor_id)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (successor_id == block_id) {
					matches++;
				}
			}
			if (matches != 1) {
				return matches > 1 ? ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE
					: ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

static void zend_mir_dominance_initialize_sets(
		zend_mir_dominance *dominance, uint32_t entry_index)
{
	uint32_t block;

	for (block = 0; block < dominance->block_count; block++) {
		uint64_t *set = zend_mir_dominance_set(dominance, block);
		uint32_t member;
		if (!dominance->reachable[block] || block == entry_index) {
			zend_mir_dominance_set_member(dominance, set, block);
			continue;
		}
		for (member = 0; member < dominance->block_count; member++) {
			if (dominance->reachable[member]) {
				zend_mir_dominance_set_member(dominance, set, member);
			}
		}
	}
}

static zend_mir_cfg_status zend_mir_dominance_iterate(zend_mir_dominance *dominance,
		const zend_mir_view *view, uint32_t entry_index, uint64_t *temporary)
{
	bool changed;

	do {
		uint32_t block;
		changed = false;
		for (block = 0; block < dominance->block_count; block++) {
			uint32_t predecessor_count;
			uint32_t slot;
			bool have_predecessor = false;
			uint64_t *current;
			if (!dominance->reachable[block] || block == entry_index) {
				continue;
			}
			memset(temporary, 0xff,
				dominance->word_count * sizeof(*temporary));
			predecessor_count = view->predecessor_count(
				view->context, dominance->block_ids[block]);
			for (slot = 0; slot < predecessor_count; slot++) {
				zend_mir_block_id predecessor_id;
				int predecessor;
				uint32_t word;
				if (!view->predecessor_at(view->context,
						dominance->block_ids[block], slot, &predecessor_id)) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				predecessor = zend_mir_dominance_find(dominance, predecessor_id);
				if (predecessor < 0) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				if (!dominance->reachable[predecessor]) {
					continue;
				}
				for (word = 0; word < dominance->word_count; word++) {
					temporary[word] &= zend_mir_dominance_set(
						dominance, (uint32_t) predecessor)[word];
				}
				have_predecessor = true;
			}
			if (!have_predecessor) {
				return ZEND_MIR_CFG_STATUS_INVALID_CFG;
			}
			zend_mir_dominance_set_member(dominance, temporary, block);
			current = zend_mir_dominance_set(dominance, block);
			if (memcmp(current, temporary,
					dominance->word_count * sizeof(*current)) != 0) {
				memcpy(current, temporary,
					dominance->word_count * sizeof(*current));
				changed = true;
			}
		}
	} while (changed);
	return ZEND_MIR_CFG_STATUS_OK;
}

static zend_mir_cfg_status zend_mir_dominance_compute_idoms(
		zend_mir_dominance *dominance, uint32_t entry_index)
{
	uint32_t block;

	for (block = 0; block < dominance->block_count; block++) {
		uint32_t candidate;
		bool found = false;
		dominance->immediate_dominators[block] = ZEND_MIR_ID_INVALID;
		if (!dominance->reachable[block] || block == entry_index) {
			continue;
		}
		for (candidate = 0; candidate < dominance->block_count; candidate++) {
			uint32_t other;
			bool immediate = true;
			if (candidate == block
					|| !zend_mir_dominance_set_contains(dominance, block, candidate)) {
				continue;
			}
			for (other = 0; other < dominance->block_count; other++) {
				if (other == block || other == candidate
						|| !zend_mir_dominance_set_contains(dominance, block, other)) {
					continue;
				}
				if (!zend_mir_dominance_set_contains(dominance, candidate, other)) {
					immediate = false;
					break;
				}
			}
			if (immediate) {
				if (found) {
					return ZEND_MIR_CFG_STATUS_INVALID_CFG;
				}
				dominance->immediate_dominators[block]
					= dominance->block_ids[candidate];
				found = true;
			}
		}
		if (!found) {
			return ZEND_MIR_CFG_STATUS_INVALID_CFG;
		}
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_dominance_create(zend_mir_dominance **out,
		const zend_mir_view *view, zend_mir_function_id function_id,
		zend_mir_allocator allocator, zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_dominance *dominance;
	zend_mir_function_record function;
	zend_mir_cfg_status status;
	uint32_t *queue;
	uint64_t *temporary;
	size_t set_count;
	int entry;

	if (out == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	*out = NULL;
	if (!zend_mir_dominance_callbacks_valid(view) || allocator.allocate == NULL
			|| allocator.reset == NULL || !zend_mir_id_is_valid(function_id)) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	if (!zend_mir_contract_is_compatible(view->contract_version)) {
		return ZEND_MIR_CFG_STATUS_INCOMPATIBLE_CONTRACT;
	}
	if (!zend_mir_dominance_find_function(view, function_id, &function)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	dominance = (zend_mir_dominance *) zend_mir_dominance_allocate(
		allocator, 1, sizeof(zend_mir_dominance), _Alignof(zend_mir_dominance),
		&status);
	if (dominance == NULL) {
		return status;
	}
	memset(dominance, 0, sizeof(*dominance));
	dominance->allocator = allocator;
	status = zend_mir_dominance_collect_blocks(dominance, view, function_id);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	status = zend_mir_dominance_validate_edges(dominance, view);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	dominance->word_count = dominance->block_count / 64
		+ (dominance->block_count % 64 != 0);
	if (!zend_mir_dominance_size(dominance->block_count,
		dominance->word_count, &set_count)) {
		status = ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED;
		goto fail;
	}
	dominance->reachable = (bool *) zend_mir_dominance_allocate(allocator,
		dominance->block_count, sizeof(bool), _Alignof(bool), &status);
	if (dominance->reachable == NULL) {
		goto fail;
	}
	dominance->immediate_dominators = (zend_mir_block_id *) zend_mir_dominance_allocate(
		allocator, dominance->block_count, sizeof(zend_mir_block_id),
		_Alignof(zend_mir_block_id), &status);
	if (dominance->immediate_dominators == NULL) {
		goto fail;
	}
	dominance->sets = (uint64_t *) zend_mir_dominance_allocate(allocator,
		set_count, sizeof(uint64_t), _Alignof(uint64_t), &status);
	if (dominance->sets == NULL) {
		goto fail;
	}
	queue = (uint32_t *) zend_mir_dominance_allocate(allocator,
		dominance->block_count, sizeof(uint32_t), _Alignof(uint32_t), &status);
	if (queue == NULL) {
		goto fail;
	}
	temporary = (uint64_t *) zend_mir_dominance_allocate(allocator,
		dominance->word_count, sizeof(uint64_t), _Alignof(uint64_t), &status);
	if (temporary == NULL) {
		goto fail;
	}
	memset(dominance->reachable, 0,
		dominance->block_count * sizeof(*dominance->reachable));
	memset(dominance->sets, 0, set_count * sizeof(*dominance->sets));
	status = zend_mir_dominance_mark_reachable(
		dominance, view, function.entry_block_id, queue);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	entry = zend_mir_dominance_find(dominance, function.entry_block_id);
	if (entry < 0) {
		status = ZEND_MIR_CFG_STATUS_INVALID_CFG;
		goto fail;
	}
	zend_mir_dominance_initialize_sets(dominance, (uint32_t) entry);
	status = zend_mir_dominance_iterate(dominance, view, (uint32_t) entry, temporary);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	status = zend_mir_dominance_compute_idoms(dominance, (uint32_t) entry);
	if (status != ZEND_MIR_CFG_STATUS_OK) {
		goto fail;
	}
	*out = dominance;
	return ZEND_MIR_CFG_STATUS_OK;

fail:
	zend_mir_dominance_emit(diagnostics, function_id,
		status == ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED
			? ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED
			: status == ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED
				? ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED
				: ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
		"cannot compute deterministic dominance");
	allocator.reset(allocator.context);
	return status;
}

void zend_mir_dominance_destroy(zend_mir_dominance *dominance)
{
	zend_mir_reset_fn reset;
	void *context;

	if (dominance == NULL) {
		return;
	}
	reset = dominance->allocator.reset;
	context = dominance->allocator.context;
	reset(context);
}

bool zend_mir_dominance_is_reachable(const zend_mir_dominance *dominance,
		zend_mir_block_id block_id)
{
	int index = zend_mir_dominance_find(dominance, block_id);

	return index >= 0 && dominance->reachable[index];
}

bool zend_mir_dominates(const zend_mir_dominance *dominance,
		zend_mir_block_id dominator, zend_mir_block_id block_id)
{
	int dominator_index = zend_mir_dominance_find(dominance, dominator);
	int block_index = zend_mir_dominance_find(dominance, block_id);

	if (dominator_index < 0 || block_index < 0) {
		return false;
	}
	/* An unreachable block has no path dominators, but still dominates itself. */
	if (!dominance->reachable[block_index]) {
		return dominator_index == block_index;
	}
	return zend_mir_dominance_set_contains(
		dominance, (uint32_t) block_index, (uint32_t) dominator_index);
}

zend_mir_block_id zend_mir_immediate_dominator(
		const zend_mir_dominance *dominance, zend_mir_block_id block_id)
{
	int index = zend_mir_dominance_find(dominance, block_id);

	return index < 0 ? ZEND_MIR_ID_INVALID
		: dominance->immediate_dominators[index];
}
