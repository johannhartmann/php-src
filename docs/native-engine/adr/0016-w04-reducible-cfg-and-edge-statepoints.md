# ADR 0016: Reducible CFGs and edge statepoints

## Status

Accepted.

## Decision

W04 accepts only reachable, reducible CFGs without protected regions. Backedges
must target a source loop header and agree with source dominator metadata.
Irreducible loops, try/catch/finally regions, stackless entries, and unsupported
PHI/Pi constraints fail closed with stable diagnostics.

An interrupt check belongs to the source edge on which Zend would perform it.
Every source edge flagged `interrupt_boundary` maps to exactly one ZNMIR
`STATEPOINT` on that edge before control enters the destination. Edges without
that flag must not acquire an interrupt statepoint. The statepoint carries the
source position and complete frame state required by the frozen W01 contract.

Stage-3 verification compares the source view, canonical MIR, and a
process-local source-to-MIR map before the module is returned. The map is
destroyed with lowering state and is never serialized or stored in a MIR
record.
