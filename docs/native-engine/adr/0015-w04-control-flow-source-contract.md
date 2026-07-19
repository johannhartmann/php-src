# ADR 0015: W04 control-flow source contract

## Status

Accepted.

## Decision

W04 extends the pointer-free lowering source view with stable source block,
edge, PHI, and PHI-input records. Original Zend CFG and SSA indices are the
identities; process addresses are never identities.

Source ordering is semantic. A block's successor index is the corresponding
`zend_basic_block.successors` index. An edge's predecessor index is the slot in
`zend_cfg.predecessors`. PHI input N belongs to predecessor N. Opcodes carry
their source block ID.

ZNMIR `COND_BRANCH` successor 0 is true and successor 1 is false. Zend `JMPZ`
source successor 0 is the false jump target and successor 1 is true
fallthrough. Zend `JMPNZ` source successor 0 is the true jump target and
successor 1 is false fallthrough. Lowering records the permutation rather than
reordering source edges.

## Compatibility

W03 remains contract 1.2 and retains its existing records, diagnostics, and
guarantee mask. The additive W04 source/control-flow boundary is contract 1.3.
W04 implementation must reject an incomplete 1.3 callback table; it must not
infer missing CFG facts.
