# ADR 0003: Native baseline tier and native resume targets

## Status

Accepted on 2026-07-17.

## Context

Full cutover needs a low-complexity execution tier that covers PHP semantics
without using VM dispatch as a safety net. Exceptions, interrupts, reentrancy,
and future optimized exits also need stable places to reconstruct execution.

## Decision

Provide a native baseline tier as the completeness tier for accepted execution
units. It consumes verified ZNMIR and emits native code without requiring an
optimizing tier. Represent every resumable point as an explicit native resume
target with a verified frame state and immutable code-version identity.

Resume targets may enter baseline native code, a dedicated native runtime
continuation, or a caller boundary defined by the ABI contract. They must not
enter a generic VM opcode-dispatch loop.

## Consequences

- Semantic coverage belongs in the baseline tier before optional optimization.
- Resume metadata becomes part of code-version validation and lifetime.
- Optimized tiers can transfer to known baseline-native points without
  inventing a second frame model.
- Missing resume metadata makes a unit ineligible for publication.

## Alternatives

- Using the interpreter as the baseline tier was rejected by the full-cutover
  decision.
- Resuming at arbitrary machine offsets was rejected because such offsets do not
  prove a reconstructible Zend-compatible state.

## Verification impact

Tests must force every resume class, validate live values and observable state,
and prove that targets belong to the active immutable code version. Static and
review checks must reject opcode-dispatch calls from resume paths.
