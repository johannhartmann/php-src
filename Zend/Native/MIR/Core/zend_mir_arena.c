/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_arena.h"

#include <stdalign.h>
#include <stdint.h>

struct _zend_mir_arena_chunk {
	zend_mir_arena_chunk *next;
	size_t capacity;
	size_t used;
	size_t data_offset;
	size_t alignment;
};

static bool zend_mir_is_power_of_two(size_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static bool zend_mir_align_size(size_t value, size_t alignment, size_t *out)
{
	size_t mask;

	if (out == NULL || !zend_mir_is_power_of_two(alignment)) {
		return false;
	}
	mask = alignment - 1;
	if (value > SIZE_MAX - mask) {
		return false;
	}
	*out = (value + mask) & ~mask;
	return true;
}

bool zend_mir_arena_init(zend_mir_arena *arena, const zend_mir_allocator *allocator,
		size_t default_chunk_size)
{
	if (arena == NULL || allocator == NULL || allocator->allocate == NULL
			|| allocator->reset == NULL) {
		return false;
	}
	if (default_chunk_size == 0) {
		default_chunk_size = ZEND_MIR_CORE_DEFAULT_CHUNK_SIZE;
	}
	arena->allocator = *allocator;
	arena->first = NULL;
	arena->last = NULL;
	arena->default_chunk_size = default_chunk_size;
	arena->failed = false;
	return true;
}

static void *zend_mir_arena_allocate_chunk(zend_mir_arena *arena, size_t size,
		size_t alignment)
{
	zend_mir_arena_chunk *chunk;
	size_t chunk_alignment = alignment;
	size_t data_offset;
	size_t capacity;
	size_t total_size;
	void *allocation;

	if (chunk_alignment < alignof(max_align_t)) {
		chunk_alignment = alignof(max_align_t);
	}
	if (!zend_mir_align_size(sizeof(zend_mir_arena_chunk), chunk_alignment, &data_offset)) {
		return NULL;
	}
	capacity = arena->default_chunk_size;
	if (capacity < size) {
		capacity = size;
	}
	if (data_offset > SIZE_MAX - capacity) {
		return NULL;
	}
	total_size = data_offset + capacity;
	allocation = arena->allocator.allocate(
		arena->allocator.context, total_size, chunk_alignment);
	if (allocation == NULL || ((uintptr_t) allocation & (chunk_alignment - 1)) != 0) {
		return NULL;
	}
	chunk = (zend_mir_arena_chunk *) allocation;
	chunk->next = NULL;
	chunk->capacity = capacity;
	chunk->used = size;
	chunk->data_offset = data_offset;
	chunk->alignment = chunk_alignment;
	if (arena->last == NULL) {
		arena->first = chunk;
	} else {
		arena->last->next = chunk;
	}
	arena->last = chunk;
	return (unsigned char *) allocation + data_offset;
}

void *zend_mir_arena_allocate(zend_mir_arena *arena, size_t size, size_t alignment)
{
	zend_mir_arena_chunk *chunk;
	size_t offset;

	if (arena == NULL || arena->failed || size == 0
			|| !zend_mir_is_power_of_two(alignment)) {
		if (arena != NULL) {
			arena->failed = true;
		}
		return NULL;
	}
	chunk = arena->last;
	if (chunk != NULL && chunk->alignment >= alignment
			&& zend_mir_align_size(chunk->used, alignment, &offset)
			&& offset <= chunk->capacity && size <= chunk->capacity - offset) {
		chunk->used = offset + size;
		return (unsigned char *) chunk + chunk->data_offset + offset;
	}
	if (size > SIZE_MAX - (alignment - 1)) {
		arena->failed = true;
		return NULL;
	}
	{
		void *result = zend_mir_arena_allocate_chunk(arena, size, alignment);

		if (result == NULL) {
			arena->failed = true;
		}
		return result;
	}
}

void zend_mir_arena_release(zend_mir_arena *arena)
{
	zend_mir_allocator allocator;

	if (arena == NULL || arena->allocator.reset == NULL) {
		return;
	}
	allocator = arena->allocator;
	allocator.reset(allocator.context);
}
