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

#ifndef ZEND_MIR_CORE_ARENA_H
#define ZEND_MIR_CORE_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../zend_mir.h"

typedef struct _zend_mir_arena_chunk zend_mir_arena_chunk;

/* Process-local allocation state. It is never persistent MIR identity. */
typedef struct _zend_mir_arena {
	zend_mir_allocator allocator;
	zend_mir_arena_chunk *first;
	zend_mir_arena_chunk *last;
	size_t default_chunk_size;
	bool failed;
} zend_mir_arena;

typedef enum _zend_mir_module_state {
	ZEND_MIR_MODULE_BUILDING = 0,
	ZEND_MIR_MODULE_FINALIZED = 1,
	ZEND_MIR_MODULE_FAILED = 2
} zend_mir_module_state;

/* Limits are counts. UINT32_MAX permits every valid sequential 32-bit ID. */
typedef struct _zend_mir_core_limits {
	uint32_t functions;
	uint32_t blocks;
	uint32_t instructions;
	uint32_t values;
	uint32_t constants;
	uint32_t operands;
} zend_mir_core_limits;

#define ZEND_MIR_CORE_DEFAULT_CHUNK_SIZE ((size_t) 4096)

bool zend_mir_arena_init(zend_mir_arena *arena, const zend_mir_allocator *allocator,
	size_t default_chunk_size);
void *zend_mir_arena_allocate(zend_mir_arena *arena, size_t size, size_t alignment);
void zend_mir_arena_release(zend_mir_arena *arena);

zend_mir_core_limits zend_mir_core_default_limits(void);

zend_mir_module *zend_mir_module_create(zend_mir_module_id module_id,
	const zend_mir_allocator *allocator, size_t chunk_size,
	const zend_mir_core_limits *limits, zend_mir_diagnostic_sink *diagnostics);
bool zend_mir_module_finalize(zend_mir_module *module);
void zend_mir_module_destroy(zend_mir_module *module);
zend_mir_module_state zend_mir_module_get_state(const zend_mir_module *module);
const zend_mir_view *zend_mir_module_get_view(const zend_mir_module *module);
zend_mir_mutator *zend_mir_module_get_mutator(zend_mir_module *module);

bool zend_mir_core_id_validate(uint32_t id);
bool zend_mir_core_value_id_decode(zend_mir_value_id id, bool *synthetic,
	uint32_t *payload);

#endif /* ZEND_MIR_CORE_ARENA_H */
