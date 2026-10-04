# ADR 0003: Generic native code and native resume targets

## Status

Accepted.

## Context

Every PHP function must execute natively without VM dispatch as a safety net.
Exceptions, interrupts, reentrancy, generators and failed speculation need
stable places to continue execution.

## Decision

Every compiled function has a generic native version that covers all PHP
semantics of the function without speculation. Specialized versions
(ADR 0025) deoptimize into it. Every resumable point is an explicit native
resume target with a verified frame state and immutable code-version identity.

Resume targets enter generic native code, a native runtime continuation, or a
caller boundary defined by the ABI contract. They never enter a VM
opcode-dispatch loop.

## Consequences

- Semantic coverage lives in the generic native version; specialization only
  adds faster paths in front of it.
- Resume metadata is part of code-version validation and lifetime.
- Specialized code transfers to known generic native points through one frame
  model.
- Missing resume metadata makes a unit ineligible for publication.

## Alternatives

- Resuming at arbitrary machine offsets does not prove a reconstructible
  Zend-compatible state.

## Verification impact

Tests force every resume class, validate live values and observable state,
and prove that targets belong to the active immutable code version.
