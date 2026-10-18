# ADR 0001: Atomic full cutover without production VM fallback

## Status

Accepted.

## Context

Allowing native and VM execution to alternate after publication would create two
production semantics, obscure missing native behavior, and make frame, bailout,
and observer invariants depend on an implicit escape path.

## Decision

Cut a supported execution unit over atomically from its old entry to a validated
native code version. After publication, production execution must not fall back
to an opcode-dispatch loop. Reject or fail compilation before publication when
the unit cannot satisfy the native contract: the executor throws an `Error`
for a function it cannot compile and never interprets it. Stock PHP serves as
the reference in differential tests (ADR 0008), never as a native runtime
recovery mechanism.

## Consequences

- Publication requires complete validation and a failure-atomic entry update.
- Unsupported behavior is visible before cutover instead of being masked at
  runtime.
- Deoptimization and bailout resume at explicit native resume targets governed
  by the frame contract; they do not resume through generic opcode dispatch.
- Replacing a function's code publishes another validated immutable version at
  its entry cell (ADR 0005); an active frame is never switched to VM
  execution.

## Alternatives

- Per-opcode VM fallback was rejected because it creates a second semantic path
  and conceals incomplete compilation.
- Gradual mixed dispatch in production was rejected because frame and observer
  behavior would depend on which engine happened to execute each opcode.

## Verification impact

Tests must prove publication atomicity, explicit rejection of unsupported units,
the absence of runtime opcode dispatch from native helpers, and differential
equivalence for every accepted unit. Builds with the test bridge
(`--enable-native-mir-test`) count VM handler, opline handler and `execute_ex`
entries during native execution through the probes in `Zend/zend_vm_probe.h`.
Reviews must trace every failure edge before and after entry publication.
