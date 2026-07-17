# ADR 0002: One canonical ZNMIR

## Status

Accepted on 2026-07-17.

## Context

Multiple semantic IRs or target-shaped IR variants would duplicate PHP
semantics, fragment verification, and allow platform backends to disagree about
effects, ownership, or recovery state.

## Decision

Use exactly one canonical, architecture-independent PHP MIR, named ZNMIR. ZNMIR
represents control flow, PHP operations, side effects, ownership, exceptional
edges, and complete frame states explicitly. Target lowering consumes verified
ZNMIR and may create backend-local machine representations, but those are not
alternative PHP semantic IRs.

Every ZNMIR unit must pass a verifier before lowering. The verifier rejects
unknown operations, malformed control flow, inconsistent ownership, incomplete
frame states, and effects unsupported by the selected compilation contract.

## Consequences

- All targets share one semantic definition and one primary verifier.
- Architecture details stay in backend lowering.
- Adding a PHP operation requires defining its effects, ownership, frame-state
  behavior, verifier rules, and differential coverage together.
- Backend-local optimizations may not reinterpret PHP semantics.

## Alternatives

- A separate high-level and low-level PHP MIR was rejected because semantic
  information could be lost between independently evolving contracts.
- Target-specific MIR dialects were rejected because they multiply semantic
  oracles and make cross-target comparison unreliable.

## Verification impact

Verifier tests must cover each rejection rule and every operation. Structural
checks must keep target registers, encodings, and calling conventions out of
ZNMIR. Differential tests must run the same source semantics through each
supported backend.
