# Bailout, exception, suspend, and resume ABI

This document is normative. Its declarations are pseudo-C for the contract;
they are not proposed public headers and do not fix concrete structure layout.

## Transfer classes

| Transfer | Published state | Control transfer | Cleanup owner |
|---|---|---|---|
| Normal return | Canonical result or `UNDEF`, parent frame, successor opline | Direct native return/continuation | Callee completes its obligations; result ownership moves to caller. |
| PHP exception | `EG(exception)`, throwing opline | Explicit native branch to exception continuation | The frame's native exception path releases native-owned values, then frame/unwind metadata releases frame-owned ones, each exactly once. |
| Zend bailout | Active bailout catcher plus reachable non-memory obligations | Nonlocal longjmp-style transfer | Catcher-owned recovery state; local continuation is unreachable. |
| Generator suspend | Persistent generator frame, yielded state, roots, resume ID, code-version reference | Return to generator caller | Generator suspend record. |
| Fiber switch | Captured VM/fiber state, active frame chain, roots, transfer reason | Context transfer | Suspended fiber state. |
| Native resume/deopt | Validated persistent state or deoptimized frame, resume ID, immutable active code version | Single native entry then direct registered continuation | Reconstructed frame or destination continuation. |

## PHP exception protocol

PHP exceptions are executor state, not C++ unwinding:

1. Before a throwing call or explicit throw, publish the state the boundary
   class reads under the [safepoint protocol](safepoint-contract.md).
2. Preserve the responsible user-opline index as the throwing position.
3. Publish or observe the exception through `EG(exception)`. Zend currently
   records the pre-dispatch opline and then installs its exception sentinel in
   [`zend_exceptions.c`](../../../../Zend/zend_exceptions.c#L155).
4. Test `EG(exception)` on every locally returning may-throw edge and branch to
   that frame's verified native exception continuation.
5. Use the throwing index to select try/catch/finally and the exact pending-call
   and live-value cleanup set. The native exception path releases the
   frame's native-owned live values in Zend's live-range order; frame-owned
   values follow Zend's frame cleanup. Do not advance to the normal successor.
6. When propagating to a parent, finish or transfer each cleanup obligation once,
   restore the canonical parent, and preserve Zend exception identity.

An internal helper that reports failure without `EG(exception)` follows its
documented normal error result. A helper that sets `EG(exception)` always takes
the exception edge even if it also returns a success-shaped C value.

## Zend bailout protocol

`zend_try` installs an executor-global jump buffer and `_zend_bailout` clears
`EG(current_execute_data)` before `LONGJMP`, as shown in
[`zend.h`](../../../../Zend/zend.h#L273) and
[`zend.c`](../../../../Zend/zend.c#L1264). Therefore bailout is not a status code
and code after a may-bailout call is not guaranteed to run.

Before a may-bailout boundary:

- keep the frame header chain and responsible oplines current (they are
  always current under the safepoint contract);
- native-owned PHP values need no action: after a bailout Zend drops the
  frames without releasing their values, and objects reach their destructors
  through the object store during shutdown, exactly as for frame-owned values
  in stock PHP;
- move every required non-memory cleanup action to a record reachable by the
  active catcher, or complete it before the call;
- ensure no C++ automatic object whose destructor is required for correctness
  remains live across the call;
- ensure locks, pin counts, code-version references, temporary buffers, and
  partially initialized Zend values have an explicit catcher-owned release
  action;
- mark the local normal/exception continuations as distinct from the nonlocal
  bailout continuation.

On catch, recovery consumes the published obligations, restores a consistent
executor boundary as required by the owning runtime operation, and never jumps
back into the abandoned native activation. Cleanup must tolerate
`EG(current_execute_data) == NULL`.

## Destructor and reentry protocol

Destroying or overwriting a refcounted value is a safepoint whenever its release
can invoke an object destructor or related user hook. Before release, the
value has its single owner and the release obligation is reachable outside
volatile local state.

The destructor may call PHP, allocate, mutate aliased values and heap
objects, throw, switch a fiber, or bail out. It cannot name the caller's
non-alias locals. If it returns locally, native code reloads
`EG(current_execute_data)`, `EG(exception)`, the frame opline, alias-observable
slots and every heap fact the release may invalidate; native-owned non-alias
locals stay valid. If an exception exists, the exception edge wins; if bailout
occurred, no local code executes. The obligation is completed exactly once
even when the destructor reenters the same logical operation.

Observer and interrupt callbacks use the same reentry discipline. Their
execute-data argument, arguments and responsible opline are canonical before
entry; alias-observable slots and heap facts are reloaded after local return.

## Persistent suspension state

Suspension transfers ownership out of the active native activation. A
persistent record contains at least:

- function/`op_array` identity and immutable code-version reference;
- suspended opline index and registered resume ID;
- canonical execute-data fields and logical parent policy;
- all live argument, CV, VAR/TMP, `$this`, return, and pending-call state;
- explicit roots and cleanup obligations;
- exception and normal-return continuations;
- transfer kind and generator/fiber identity.

No persistent record stores a raw generated-code address. A code version remains
retained while any persistent record names one of its resume IDs.

### Generator state machine

`created -> running -> suspended -> running -> completed`

- On first entry, the generator frame becomes canonical and current.
- At yield, materialize yielded key/value, send target, frame slots, frozen
  pending calls, roots, cleanup, suspension opline, resume ID, and code version.
  Transfer their ownership to the generator record before returning.
- On resume, reject a completed or already-running generator. Validate the
  resume ID/version, restore the caller-facing parent chain, make the generator
  current, rebuild roots, then enter the registered successor.
- On exception or close, run generator-owned cleanup and finally paths from the
  saved throwing/suspension state, then complete the record.

Zend's generator implementation restores the execute-data chain and frozen
calls before execution in
[`zend_generators.c`](../../../../Zend/zend_generators.c#L762); the native
contract preserves that observable ordering while using a native resume ID.

### Fiber state machine

`created -> running -> suspended -> running -> terminated`

- Before switching out, capture the VM stack, stack bounds, current execute
  data, active bailout catcher, active fiber, frame chain, roots, obligations,
  responsible opline, and transfer reason.
- The suspended fiber owns this captured state. The destination state becomes
  current as one atomic logical transition before observers run.
- Resume validates the persistent frame and code version before it changes the
  destination to running.
- A bailout flag crosses the context boundary as a bailout transfer and is
  forwarded to the destination catcher; it is not converted to normal return.
- Termination completes captured obligations once and publishes the result or
  exception to the resumer.

The current fiber implementation's VM-state capture includes the VM stack,
current execute data, bailout buffer, and active fiber in
[`zend_fibers.c`](../../../../Zend/zend_fibers.c#L105).

## Single-entry native resume

All generator, fiber, and future deoptimization resumes use one logical entry:

```c
typedef struct native_resume_request {
    zend_execute_data *execute_data;
    const native_code_version *expected_version;
    native_suspend_state *persistent_state;
    uint32_t resume_id;
    native_transfer_reason reason;
} native_resume_request;

native_resume_result native_resume_entry(const native_resume_request *request);
```

The concrete implementation remains private and C-compatible. The entry
performs these ordered checks before transferring control:

1. The request and persistent state are well formed and the state is not
   already running or completed.
2. The expected code version is the exact immutable version retained by the
   persistent state and remains eligible for entry.
3. `resume_id` exists in that version's read-only resume table and accepts the
   requested transfer reason and suspend kind.
4. The resume record's frame recipe, roots, cleanup, parent frames, function,
   opline target, and continuations validate against the table entry.
5. Reconstruction completes and publishes `EG(current_execute_data)`; the
   state changes to running.
6. Control transfers directly to the native block registered for the ID.

Conceptually, a table entry contains:

```c
typedef struct native_resume_descriptor {
    uint32_t resume_id;
    uint32_t target_opline_index;
    native_resume_kind kind;
    const native_frame_recipe *frame_recipe;
    native_block_identity target_block;
} native_resume_descriptor;
```

`native_block_identity` is verified code-version metadata, not an externally
supplied address. Resume IDs are local to one immutable code version. The entry
does not accept a machine offset, compute a target from user-controlled data,
or invoke the VM opcode dispatcher.

## Resume failure

Before state publication, invalid state, an unknown ID, a version mismatch, an
inactive version, a duplicate resume, or a failed frame recipe rejects the
resume through the caller boundary's defined error/exception result. The
persistent state remains suspended and owned.

After publication, failures use only the descriptor's explicit exception or
bailout continuation. They cannot fall through to another resume target.
Missing metadata makes a code version ineligible for publication, so production
execution never repairs a resume by interpreting the saved opcode.

## Deoptimization

A specialized code version ([ADR 0025](../../adr/0025-native-canonical-execution.md))
produces the same request after moving ownership of every value its guard's
frame-state map names into the frame slots. It uses the same frame,
version identity, roots, cleanup, and single entry; there is no second resume
ABI.

- **Semantic continuation:** a resume ID names the continuation that follows
  the work already done, not just a source position. Side effects, argument
  evaluation, `RECV` processing and releases performed before the guard are
  never repeated; a guard therefore precedes every side effect of its
  operation, or its resume ID names the successor of the completed part.
- **Invalidation:** code lifetime and assumption validity are separate. When
  an assumption (class layout, function or constant binding, declaration
  epoch) is invalidated, no operation that depends on it runs afterwards in
  any activation: such operations re-check the assumption's generation, or
  the invalidation deoptimizes the affected activations before they reach
  one. Retired code stays mapped while an activation uses it.
- **Bounded specialization:** each function and site keeps a bounded number
  of specialized assumptions. A site whose guard keeps failing gives up that
  assumption in the next version instead of adding variants. Array shape
  facts (packed, hash) do not imply that a bucket address or shape survives a
  mutation.

This contract reserves no on-stack replacement protocol beyond these resume
entries.
