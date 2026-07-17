# ADR 0010: Bailout, exception, suspend, and resume ABI

## Status

Accepted on 2026-07-17.

## Context

Native execution must preserve four transfers that are not ordinary local
control flow: PHP exceptions, Zend bailout, generator suspension, and fiber
switching. They require explicit ownership and continuation rules. Resume also
needs a stable native target without relying on opcode dispatch or an address
inside generated machine code.

## Decision

PHP exceptions use `EG(exception)` and explicit native control-flow edges.
Native code records the throwing opline, canonicalizes the frame, and branches
to the verified exception continuation. C++ exception propagation is not part
of this ABI.

Zend bailout is a nonlocal, longjmp-style transfer, never a returned status.
Before any operation that may bail out, reenter Zend, or invoke a destructor,
native code materializes all roots and transfers cleanup responsibility to
state reachable by the active bailout catcher. No C++ object requiring RAII
destruction may remain live across such an operation.

Generator and fiber suspension store persistent, rooted frame state. Resume
enters one native dispatcher with a resume ID and the expected immutable
code-version identity. The dispatcher validates both, reconstructs the
canonical frame, and transfers directly to the registered native continuation.
It never dispatches an opcode and never accepts an arbitrary machine offset.

The normative state machines and pseudo-C boundary are in the
[resume ABI](../semantics/frames/resume-abi.md). Frame requirements at each edge
are in the [safepoint contract](../semantics/frames/safepoint-contract.md).

## Consequences

- Exception and bailout paths remain distinct and fail closed when their
  metadata is incomplete.
- Cleanup is correct even when a helper never returns locally.
- Suspended state owns its values and code-version reference until resume or
  destruction.
- Retired or mismatched code versions cannot be resumed.
- Baseline and future optimized tiers share one resume entry and frame model.

## Alternatives

- Mapping PHP exceptions to C++ exceptions was rejected because Zend exposes
  exception state through executor globals and C control flow.
- Modeling bailout as a helper return code was rejected because `_zend_bailout`
  performs a nonlocal transfer and clears `EG(current_execute_data)`.
- Direct entry at saved machine addresses was rejected because an address does
  not prove frame shape, code lifetime, or continuation identity.
- Resuming through the VM opcode loop was rejected by the native full-cutover
  and native-resume decisions.

## Verification impact

Schema validation and tests must reject version mismatches, non-dispatcher
entry, VM dispatch, and machine-offset targets. Reviews must prove that every
may-bailout, destructor, observer, interrupt, suspend, and resume edge has
canonical roots, cleanup, opline, and parent-frame state.
