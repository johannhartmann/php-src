# ADR 0013: Proof-closed scalar lowering

## Status

Accepted.

## Decision

W03 accepts only source opcodes listed as required or conditional in the
generated profile. Conditional acceptance requires all named, immutable facts.
Absence and contradiction fail closed; neither runtime speculation nor a VM
fallback fills a proof gap.

The profile is generated from every active entry in the W01 matrix, including
its source references and conservative effects. The current source count is
212, superseding the older planning count of 210.

## Consequences

The initial surface is intentionally small but auditable. Later waves own
control flow, calls and runtime effects, references, compound values, and
suspension.
