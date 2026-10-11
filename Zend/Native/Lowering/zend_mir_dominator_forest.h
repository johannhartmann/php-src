#ifndef ZEND_MIR_DOMINATOR_FOREST_H
#define ZEND_MIR_DOMINATOR_FOREST_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../MIR/zend_mir_ids.h"

/*
 * A dominator forest numbered by zend_mir_dominator_forest_number. The
 * frontend numbers it once per compile from the Zend CFG's immediate
 * dominators, which the source view reports unchanged, and every later
 * dominance question reads it.
 */
typedef struct _zend_mir_dominator_forest {
	uint32_t count;
	uint32_t *preorder;
	uint32_t *subtree_end;
	uint32_t *roots;
} zend_mir_dominator_forest;

/*
 * Whether dominator dominates block. For an acyclic immediate-dominator
 * relation this is the answer of walking block's dominator chain until it
 * reaches dominator or a root; a block on a cycle no root reaches is
 * dominated by nothing.
 */
static inline bool zend_mir_dominator_forest_dominates(
	const zend_mir_dominator_forest *forest,
	uint32_t dominator, uint32_t block)
{
	return dominator < forest->count && block < forest->count
		&& forest->preorder[dominator] != ZEND_MIR_ID_INVALID
		&& forest->preorder[block] != ZEND_MIR_ID_INVALID
		&& forest->roots[dominator] == forest->roots[block]
		&& forest->preorder[dominator] <= forest->preorder[block]
		&& forest->preorder[block] < forest->subtree_end[dominator];
}

/*
 * Numbers a dominator forest in preorder so that block a dominates block b
 * iff roots[a] == roots[b] and preorder[a] <= preorder[b] < subtree_end[a].
 *
 * idoms[i] is the immediate dominator of block i; a value that is not a
 * block or names the block itself makes the block a root. idoms may alias
 * subtree_end: it is read before subtree_end is written. Blocks no root
 * reaches keep ZEND_MIR_ID_INVALID in all three outputs. child_offsets holds
 * count + 1 entries; children, cursor, stack and stack_cursor hold count.
 */
static inline void zend_mir_dominator_forest_number(
	uint32_t count, const uint32_t *idoms,
	uint32_t *child_offsets, uint32_t *children, uint32_t *cursor,
	uint32_t *stack, uint32_t *stack_cursor,
	uint32_t *preorder, uint32_t *subtree_end, uint32_t *roots)
{
	uint32_t time = 0;
	uint32_t i;

	memset(child_offsets, 0, ((size_t) count + 1) * sizeof(*child_offsets));
	for (i = 0; i < count; i++) {
		const uint32_t idom = idoms[i];

		preorder[i] = ZEND_MIR_ID_INVALID;
		if (idom < count && idom != i) {
			child_offsets[idom + 1]++;
			roots[i] = ZEND_MIR_ID_INVALID;
		} else {
			roots[i] = i;
		}
	}
	for (i = 1; i <= count; i++) {
		child_offsets[i] += child_offsets[i - 1];
	}
	memcpy(cursor, child_offsets, (size_t) count * sizeof(*cursor));
	for (i = 0; i < count; i++) {
		const uint32_t idom = idoms[i];

		if (idom < count && idom != i) {
			children[cursor[idom]++] = i;
		}
	}
	for (i = 0; i < count; i++) {
		subtree_end[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < count; i++) {
		uint32_t depth;

		/* Only a root names itself; the walk below names other blocks. */
		if (roots[i] != i) {
			continue;
		}
		depth = 1;
		stack[0] = i;
		stack_cursor[0] = child_offsets[i];
		preorder[i] = time++;
		while (depth != 0) {
			const uint32_t block = stack[depth - 1];
			uint32_t *next_child = &stack_cursor[depth - 1];

			if (*next_child < child_offsets[block + 1]) {
				const uint32_t child = children[(*next_child)++];

				if (preorder[child] != ZEND_MIR_ID_INVALID) {
					continue;
				}
				preorder[child] = time++;
				roots[child] = i;
				stack[depth] = child;
				stack_cursor[depth] = child_offsets[child];
				depth++;
			} else {
				subtree_end[block] = time;
				depth--;
			}
		}
	}
}

#endif /* ZEND_MIR_DOMINATOR_FOREST_H */
