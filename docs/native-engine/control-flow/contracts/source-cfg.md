# W04 source CFG contract

The source view exposes blocks, edges, PHIs, and PHI inputs only through
bounded callbacks. IDs are original stable Zend indices. All records are
pointer-free.

- Blocks enumerate in ascending original block ID.
- Opcodes enumerate in opline order and carry their owning block ID.
- Edges enumerate by `(from_block_id, successor_index)`.
- `successor_index` is the exact Zend successor slot.
- `predecessor_index` is the exact slot in `zend_cfg.predecessors`.
- PHIs enumerate by `(block_id, source_slot_kind, source_slot_index,
  result_ssa_variable_id)`.
- PHI input N names predecessor N. Pi records have one input, associated with
  their source predecessor.

Malformed counts, duplicate IDs, broken reciprocal slots, noncontiguous block
opcode ranges, out-of-range SSA IDs, or contradictory dominator/loop metadata
produce `[MIRL0014]`.
