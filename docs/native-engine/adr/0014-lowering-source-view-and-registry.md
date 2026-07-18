# ADR 0014: Deterministic and failure-atomic lowering boundary

## Status

Accepted.

## Decision

Lowering consumes immutable pointer-free source records and emits through the
canonical MIR mutator. Providers claim disjoint opcode numbers and enumerate in
semantic-family/provider order.

A module is returned only after finalization and both verifier stages succeed.
All other outcomes return a stable `ZEND_MIRL_*` diagnostic and no module.

## Consequences

Tests can supply bounded hosts without embedding the VM. Registry order cannot
depend on link or insertion order, and partial MIR never escapes failure.
