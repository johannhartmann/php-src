/* Deterministic dominance analysis over a read-only ZNMIR view. */

#ifndef ZEND_MIR_DOMINANCE_H
#define ZEND_MIR_DOMINANCE_H

#include "zend_mir_cfg.h"

typedef struct _zend_mir_dominance zend_mir_dominance;

/*
 * Uses sorted block IDs and iterative bitsets, so results do not depend on
 * source enumeration or allocator order. Worst-case construction is
 * O(V * E * ceil(V / 64) + V^3), with O(V * ceil(V / 64)) storage.
 * Unreachable blocks dominate only themselves and have no immediate
 * dominator.
 */
zend_mir_cfg_status zend_mir_dominance_create(zend_mir_dominance **out,
	const zend_mir_view *view, zend_mir_function_id function_id,
	zend_mir_allocator allocator, zend_mir_diagnostic_sink *diagnostics);
void zend_mir_dominance_destroy(zend_mir_dominance *dominance);

bool zend_mir_dominance_is_reachable(const zend_mir_dominance *dominance,
	zend_mir_block_id block_id);
bool zend_mir_dominates(const zend_mir_dominance *dominance,
	zend_mir_block_id dominator, zend_mir_block_id block_id);
zend_mir_block_id zend_mir_immediate_dominator(
	const zend_mir_dominance *dominance, zend_mir_block_id block_id);

#endif /* ZEND_MIR_DOMINANCE_H */
