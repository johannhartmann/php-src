# ADR 0001: Atomic full cutover without production VM fallback

## Status

Accepted on 2026-07-17.

## Context

Allowing native and VM execution to alternate after publication would create two
production semantics, obscure missing native behavior, and make frame, bailout,
and observer invariants depend on an implicit escape path.

## Decision

Cut a supported execution unit over atomically from its old entry to a validated
native code version. After publication, production execution must not fall back
to an opcode-dispatch loop. Reject or fail compilation before publication when
the unit cannot satisfy the native contract. Interpreter execution may remain an
explicit reference mode in differential test infrastructure, not a native
runtime recovery mechanism.

## Consequences

- Publication requires complete validation and a failure-atomic entry update.
- Unsupported behavior is visible before cutover instead of being masked at
  runtime.
- Deoptimization and bailout resume at explicit native resume targets governed
  by the frame contract; they do not resume through generic opcode dispatch.
- Rollback selects a previously validated immutable code version at the entry
  cell rather than switching an active frame to VM execution.

## Alternatives

- Per-opcode VM fallback was rejected because it creates a second semantic path
  and conceals incomplete compilation.
- Gradual mixed dispatch in production was rejected because frame and observer
  behavior would depend on which engine happened to execute each opcode.

## Verification impact

Tests must prove publication atomicity, explicit rejection of unsupported units,
the absence of runtime opcode dispatch from native helpers, and differential
equivalence for every accepted unit. Reviews must trace every failure edge before
and after entry publication.
