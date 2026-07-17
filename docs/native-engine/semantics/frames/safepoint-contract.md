# Safepoint contract

A safepoint is any boundary where Zend code, GC, an observer, a signal-driven
interrupt, reentrant PHP execution, suspension, or nonlocal control flow can
observe or retain native state. Canonicalization completes before the boundary,
not after the callee begins.

## Opline invariant

For a user frame, the contract stores `opline.index` as an index into that
frame's exact `op_array`; raw pointers are never persisted in metadata.

- **Before a user opcode boundary:** the index is the opcode responsible for
  the operation about to observe, call, allocate, destroy, throw, suspend, or
  check interrupts. `EX(opline)` points to that opcode.
- **After successful completion:** before another observable operation, the
  index advances to the semantic successor that will execute. A branch uses its
  selected successor, not necessarily `index + 1`.
- **Callee entry:** the callee index is the first opcode that will execute after
  argument receive processing. The caller remains at the call opcode until the
  call returns normally.
- **Internal callee:** an internal function has no user `op_array` or user
  opline index. Its materialized execute-data frame is current while the caller
  owns the responsible call-opline index.
- **Exception:** before publishing `EG(exception)`, the throwing index is the
  responsible opcode. Exception transfer preserves that index as the exception
  continuation even if `EX(opline)` temporarily names Zend's exception-dispatch
  sentinel. Zend records this distinction in
  [`zend_exceptions.c`](../../../../Zend/zend_exceptions.c#L206).
- **Generator suspension:** the suspended index is the `YIELD` or `YIELD_FROM`
  opcode. Its resume ID maps to the verified semantic successor. The current VM
  saves the yield opline before materializing generator state in
  [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8442).
- **Fiber suspension:** the frame keeps the responsible call/helper opline; the
  persistent fiber state owns the captured VM stack and current execute-data
  chain.
- **Resume:** validation occurs while the frame still reports `suspended` at
  the suspension index. After reconstruction and before native transfer, the
  frame reports `before` at the resume target index.

For straight-line code between safepoints, a target instruction pointer may be
ahead of `EX(opline)`. It must be synchronized to the rule above before any
observable or exceptional boundary.

## Canonicalization protocol

Every safepoint performs these ordered steps:

1. Record the responsible opline under the invariant above.
2. Materialize all live zvals and pending calls in their canonical frame or
   persistent-state locations.
3. Apply pending writes to arguments, CVs, VAR/TMP slots, `$this`, return
   storage, symbol tables, and the parent chain.
4. Publish roots and one cleanup obligation for each owned live value.
5. Publish the expected exception, return, bailout, and suspend continuations.
6. Publish `EG(current_execute_data)` for the frame the operation expects.
7. Invalidate private register caches that reentry or mutation can make stale.

On a normal return, the post-state below becomes canonical before execution
continues. On exception, bailout, or suspension, the corresponding edge owns
state and cleanup; no normal post-state is assumed.

## Required safepoints

| Class | Required pre-state | Normal post-state / nonlocal edge |
|---|---|---|
| Function entry | Callee identity, arguments, `$this`, return storage, parent, runtime cache, initialized CVs, and first executable opline are canonical; callee is current. | Body starts at that opline. Entry observer may reenter or throw, so no sole register-owned PHP value exists. |
| User call | Caller at call opline; pending callee, arguments, named arguments, return target, and both cleanup sets are canonical. | Callee becomes current with its entry opline. Normal return restores caller and advances to selected successor; exception keeps the call opline as throw context. |
| Internal call | Caller at call opline; internal execute-data frame and arguments are canonical and current before observer/handler entry. | End observer and interrupt see canonical state; arguments/results are cleaned, caller becomes current, then advances. Exception transfers with caller call context. |
| Allocation | Responsible opcode, all live refcounted values, roots, and cleanup are canonical before allocator entry. | New value is assigned to its owned result slot before another boundary; allocation failure follows exception or bailout metadata. |
| Potential destructor | Value being released plus every other live value is rooted; ownership is transferred to reachable cleanup state before decrement/destruction. | Re-read `EG(current_execute_data)`, `EG(exception)`, affected zvals, and control state; discard register caches. Reentry, exception, fiber suspension, or bailout may have occurred. |
| Exception edge | Throwing opline, parent chain, pending calls, live ranges, roots, and cleanup are canonical before `EG(exception)` is observed. | Branch to the explicit native exception continuation selected for the throwing index; unwind exactly once. |
| Bailout helper | All roots and cleanup are owned by state reachable from the active `zend_try` catcher; no required local destructor remains. | No local post-state exists. Transfer is nonlocal and the catcher performs contract-defined cleanup. |
| Observer | Observed execute-data, opline, arguments/locals, return or exception value, and call flags are canonical before callback entry. | Callback may reenter, mutate observable values, throw, or bail out; reload all observable state. |
| Interrupt | Current frame and responsible opline are canonical before the interrupt callback. | Callback may throw or bail out; on local return, reload executor globals and live zvals before advancing. Current Zend interrupt entry is shown in [`zend_execute.c`](../../../../Zend/zend_execute.c#L4314). |
| Generator suspend | Yield opcode, yielded key/value, send target, frame slots, pending call chain, roots, cleanup, parent policy, resume ID, and code version are persistent. | Generator is not active; persistent state owns transferred values until resume or destruction. |
| Generator resume | Persistent state, resume ID, code version, and caller chain are validated before the generator becomes current. | Reconstruct chain and roots, advance to the registered successor, then enter native code. Generator chain restoration is visible in [`zend_generators.c`](../../../../Zend/zend_generators.c#L762). |
| Fiber switch | Active VM stack, top/end, current execute data, bailout catcher, active fiber, roots, and responsible opline are captured. | Destination state is restored atomically; source persistent state owns its captured frames. Zend's captured fields are listed in [`zend_fibers.c`](../../../../Zend/zend_fibers.c#L105). |
| Future deopt/resume | Logical and physical frames, live values, roots, cleanup, resume ID, and immutable code version are canonical. | Enter the same version-checked native resume dispatcher. This row reserves the boundary contract, not an optimizer implementation. |

## Roots

At a safepoint, every live refcounted PHP value is reachable through one of:

- a canonical argument, CV, VAR/TMP, `$this`, return, extra-argument, or pending
  call slot;
- a persistent generator/fiber suspend record;
- an established Zend executor-global or helper-owned root documented for that
  call.

The frame-state `roots` array lists rooted slot IDs. A slot marked `rooted` but
absent from that list is invalid. Conversely, a listed ID must identify a slot
in the same frame. Native stack maps may accelerate discovery, but they cannot
be the only representation presented to existing Zend root walkers.

## Cleanup and ownership

Each live owned slot has one obligation: `destroy`, `release`, or `transfer`.
The obligation is `pending` while the frame owns it, `transferred` when a
persistent state or caller owns it, and `complete` only after the value is no
longer live. Aliases and borrowed pointers do not create a second destructor.

Cleanup ordering follows PHP evaluation and Zend live ranges. An obligation is
published before invoking anything that can throw, bail out, observe, reenter,
or suspend. Exceptional cleanup must traverse pending calls and live results in
the same semantic order used by Zend exception dispatch.

## Register discipline

Across a safepoint, registers may contain:

- unobservable non-refcounted scalar values whose reconstruction is explicit;
- raw addresses whose owners remain rooted and whose validity is rechecked;
- duplicate caches of canonical slots that are invalidated on reentry.

A register cannot be the sole owner or sole root of a zval, object, reference,
array, string, resource, pending call, return target, or continuation. No
callee-saved-register convention extends the public Zend ABI.

## Verification rule

The compiler enumerates each boundary class in metadata. Publication verifies
that a complete state exists before and after every locally returning edge and
on every exception, bailout, and suspension edge. An unclassified helper is
treated as capable of allocation, exception, destructor reentry, observation,
interrupt interaction, and bailout until a narrower reviewed contract proves
otherwise.

## Required conformance cases

Tests of a native implementation must isolate the current-engine distinctions
that a single happy-path call does not expose:

- builds that keep the instruction pointer in a VM global register still
  synchronize `EX(opline)` before every observable boundary;
- skipped `RECV` opcodes make callee entry report the first opcode that actually
  executes;
- internal frames have a null user-opline index while their caller reports the
  responsible call opcode;
- an exception preserves the throwing index while exception dispatch uses its
  sentinel, including throws from finally cleanup;
- call begin/end observers and the post-internal-call interrupt see canonical
  execute-data and return/exception state in their actual ordering;
- destructor reentry mutates an alias and throws, proving that stale register
  caches are discarded and cleanup runs once;
- generator yield/resume reconstructs frozen pending calls and enters the
  registered successor rather than re-executing the yield;
- fiber switch restores `EG(current_execute_data)`, the bailout catcher, and all
  captured roots before destination observers execute;
- future deoptimization is rejected when any logical parent, root, cleanup
  obligation, resume ID, or code-version identity is absent.
