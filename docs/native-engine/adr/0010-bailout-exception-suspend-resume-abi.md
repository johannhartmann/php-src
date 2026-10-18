# ADR 0010: Bailout, exception, suspend, and resume ABI

## Status

Accepted.

## Context

Native execution must preserve four transfers that are not ordinary local
control flow: PHP exceptions, Zend bailout, generator suspension, and fiber
switching. They require explicit ownership and continuation rules. Resume also
needs a stable native target without relying on opcode dispatch or an address
inside generated machine code.

## Decision

PHP exceptions use `EG(exception)` and explicit native control-flow edges.
Native code publishes the throwing opline and the frame state the exception
boundary reads (ADR 0025), then branches to the verified exception
continuation. A native callee reports a pending exception to its native caller
with the `ZEND_NATIVE_EXCEPTION` status. C++ exception propagation is not part
of this ABI.

Zend bailout is a nonlocal, longjmp-style transfer, never a returned status.
Before any operation that may bail out, reenter Zend, or invoke a destructor,
native code publishes the state that boundary reads and completes every
non-memory cleanup obligation or transfers it to state reachable by the
active bailout catcher. No C++ object requiring RAII destruction may remain
live across such an operation.

That state is reachable through bailout records (`zend_native_bailout_record`
in `Zend/Native/Runtime/Common/zend_native_calls.h`): a native boundary that
must clean up when a bailout passes it links a record into a per-thread chain
(one per fiber) instead of installing its own `zend_try`. `_zend_bailout`
calls `zend_native_bailout_unwind()` before its jump, which unwinds every
record linked while its target was the active catcher, newest first, while
their C frames are still intact; the bailout then continues to the catcher. A
boundary that turns a bailout into a returned status for its native callers
keeps its own `zend_try`.

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
- Generic and specialized code versions share one resume entry and frame model.

## Alternatives

- Mapping PHP exceptions to C++ exceptions was rejected because Zend exposes
  exception state through executor globals and C control flow.
- Modeling bailout as a helper return code was rejected because `_zend_bailout`
  performs a nonlocal transfer and clears `EG(current_execute_data)`.
- Direct entry at saved machine addresses was rejected because an address does
  not prove frame shape, code lifetime, or continuation identity.
- Resuming through the VM opcode loop was rejected by ADR 0001 and ADR 0003.

## Verification impact

Schema validation and tests must reject version mismatches, non-dispatcher
entry, VM dispatch, and machine-offset targets. Reviews must prove that every
may-bailout, destructor, observer, interrupt, suspend, and resume edge has
canonical roots, cleanup, opline, and parent-frame state.
