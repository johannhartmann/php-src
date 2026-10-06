#ifndef ZEND_NATIVE_INLINE_H
#define ZEND_NATIVE_INLINE_H

#include "Zend/zend_compile.h"
#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

/*
 * Tier-2 inlining (ADR 0025 section 5) by splicing: a copy of a hot
 * function whose call sites to small callees carry the callees' bodies,
 * with the original call as the fallback every body bails to.
 */
typedef void *(*zend_native_inline_alloc_t)(void *context, size_t size);

/*
 * The spliced copy of host and its inline metadata, or NULL when no call
 * site qualifies. Memory comes from alloc and lives as long as the copy.
 * persistent_callees_only admits only callees in shared memory.
 */
zend_op_array *zend_native_inline_splice(
	const zend_op_array *host, bool persistent_callees_only,
	zend_native_inline_alloc_t alloc, void *alloc_context,
	zend_native_inline_host **out_host);

/* ZEND_NATIVE_TIER2_INLINE=1 enables tier-2 inlining. */
bool zend_native_inline_enabled(void);

#endif
