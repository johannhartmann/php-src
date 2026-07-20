# ADR 0021: W05-v2 Evidence and Capability Registry

## Status

Accepted. This decision supersedes the W05-v1 named-call, default-argument,
self-call, compiler-sidechannel, evidence, and verifier-receipt rules in ADR
0019. ADR 0019 remains the historical description of the archived v1 seal.

## Decision

W05 is resealed with receipt format 2.0.0. Verification has three monotonic
levels: `metadata` validates structure and commit binding; `reproducible` also
rehashes every committed summary, review, dependency receipt, phase receipt,
profile, definition, and coverage artifact; `full` additionally rehashes the
referenced external raw logs and binary manifests from an explicit artifact
root.

Command summaries store tokenized, repository-relative commands and profile
names rather than host paths. Reference and candidate binaries are identified
by SHA-256, Git commit/tree, configure arguments, and toolchain. Reviews are
committed JSON objects and their contents are validated before their digests
count.

The standalone, wave-receipt, and task-result phase-receipt definitions are
schema-equivalent. Each summary reference binds `command_id`, path, and digest;
implementation repair phases may repeat contiguously, and reviews bind to the
last such phase. Candidate binary commit and tree values equal the receipt
subject. Dependency receipt bytes are read from that subject commit before
recursive verification, never from the verifier's current worktree.

Capability and debt strings have one authoritative registry with stable numeric
transport IDs. Persistent MIR stores sorted ID spans. W05 bit masks remain only
as a derived compatibility projection.

All final verifiers emit receipts over one module fingerprint and one source
fingerprint. W05 succeeds only with passing `structural`, `scalar`,
`control_flow`, and `call_model` receipts.

Parameter modes are an ordered table keyed by target and ordinal, with no
64-parameter ceiling. A fully supplied default-bearing target, a statically
complete compile-normalized named call, and an exact self-call are direct W05
calls. Missing defaults, placeholders, extra arguments, runtime named
containers, and by-reference semantics remain deferred.

## Consequences

The private SEND syntax-history bit and compiler modification are removed.
Receipt v1 remains readable for historical W04 and archived W05 evidence.
W05 remains model-only and is never codegen eligible.
