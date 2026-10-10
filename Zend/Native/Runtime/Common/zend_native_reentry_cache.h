#ifndef ZEND_NATIVE_REENTRY_CACHE_H
#define ZEND_NATIVE_REENTRY_CACHE_H

/*
 * The reentry cache: the ready cell, code and frame entry that
 * zend_native_reentry_resolve() found for a function's opcodes under the
 * active reentry scope. execute_ex reads a hit inline; zend_native_calls.c
 * fills and forgets the entries.
 */

#include "Zend/zend_compile.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#define ZEND_NATIVE_REENTRY_CACHE_SIZE 1024

typedef struct _zend_native_reentry_cache_entry {
	uint64_t signature;
	/* A cell of a persistent generation survives the request: it is
	 * cached under the persistent epoch, which only retiring persistent
	 * code advances. */
	bool persistent;
	const zend_op *opcodes;
	zend_native_entry_cell *cell;
	/* The cell's code when it was cached and that code's frame entry. */
	const zend_native_code *code;
	zend_native_frame_entry_t entry;
	uint64_t epoch;
	uint32_t last;
} zend_native_reentry_cache_entry;

extern ZEND_EXT_TLS zend_native_reentry_cache_entry
	zend_native_reentry_cache[ZEND_NATIVE_REENTRY_CACHE_SIZE];
extern ZEND_EXT_TLS zend_native_reentry_scope *zend_native_active_reentry_scope;
/* The call-resolution cache epoch; request-local cells are cached under
 * it, persistent ones under zend_native_reentry_persistent_epoch. */
extern uint64_t zend_native_call_resolution_cache_epoch;
extern uint64_t zend_native_reentry_persistent_epoch;

static zend_always_inline uint64_t zend_native_reentry_signature(void)
{
	return zend_native_active_reentry_scope != NULL
		? zend_native_active_reentry_scope->signature : 0;
}

static zend_always_inline zend_native_reentry_cache_entry *
zend_native_reentry_cache_slot(const zend_op *opcodes, uint64_t signature)
{
	return &zend_native_reentry_cache[
		((((uintptr_t) opcodes ^ signature)
				* UINT64_C(0x9e3779b97f4a7c15)) >> 40)
			& (ZEND_NATIVE_REENTRY_CACHE_SIZE - 1)];
}

/* The cached ready cell of a user function whose code is still the one
 * cached with its frame entry, or NULL. */
static zend_always_inline zend_native_frame_entry_t
zend_native_reentry_cached_entry(const zend_function *function,
	zend_native_entry_cell **cell, const zend_native_code **code)
{
	const uint64_t signature = zend_native_reentry_signature();
	const zend_native_reentry_cache_entry *cached =
		zend_native_reentry_cache_slot(
			function->op_array.opcodes, signature);

	if (cached->opcodes != function->op_array.opcodes
			|| cached->last != function->op_array.last
			|| cached->signature != signature
			|| cached->epoch != (cached->persistent
				? zend_native_reentry_persistent_epoch
				: zend_native_call_resolution_cache_epoch)
			|| cached->cell->state != ZEND_NATIVE_ENTRY_READY
			|| cached->entry == NULL
			|| zend_native_entry_cell_load(cached->cell) != cached->code) {
		return NULL;
	}
	*cell = cached->cell;
	*code = cached->code;
	return cached->entry;
}

#endif /* ZEND_NATIVE_REENTRY_CACHE_H */
