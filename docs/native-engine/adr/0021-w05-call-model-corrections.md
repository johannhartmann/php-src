# ADR 0021: W05 direct-call model corrections

## Status

Accepted. This decision supersedes the W05-v1 named-call, default-argument,
self-call, compiler-sidechannel, and persisted verification-result rules in
ADR 0019.

## Decision

W05 succeeds only after the final structural, scalar, control-flow, and call
model verifiers pass. The module dump is recomputed to detect mutation during
verification, but verification results and planning metadata are not persisted
in MIR.

Parameter modes are an ordered table keyed by target and ordinal, with no
64-parameter ceiling. A fully supplied default-bearing target, a statically
complete compile-normalized named call, and an exact self-call are direct W05
calls. Missing defaults, placeholders, extra arguments, runtime named
containers, and by-reference semantics remain unsupported.

## Consequences

The private SEND syntax-history bit and compiler modification are removed.
W05 remains model-only and is never codegen eligible. Source, contract,
differential, sanitizer, and fuzz tests validate the implementation directly;
there is no separate evidence-publication or delivery-workflow format.
