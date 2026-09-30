# Safepoint contract

A safepoint is any boundary where Zend code, GC, an observer, a signal-driven
interrupt, reentrant PHP execution, suspension, or nonlocal control flow can
observe or retain native state. Canonicalization completes before the boundary,
not after the callee begins.

Under [ADR 0025](../../adr/0025-native-canonical-execution.md) native state is
canonical. A safepoint publishes exactly the state its boundary class reads
(table below) and, after a locally returning boundary, reloads exactly the
state that boundary may have changed. It does not mirror every live value into
the Zend frame.

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

## Ownership model

Every live refcounted PHP value has exactly one owner location:

- **frame-owned:** its canonical frame slot (argument, CV, VAR/TMP, `$this`,
  return storage, pending call) owns the reference;
- **native-owned:** a native location (register or native stack slot described
  by the machine frame-state map) owns the reference. The value's frame slot
  then holds `IS_UNDEF` or a non-refcounted value, never a stale pointer, so
  `zend_free_compiled_variables()`, live-range cleanup and GC walkers neither
  free it a second time nor count it.

Materialization moves ownership into the slot; it never adds a reference. A
native copy of a frame-owned value is a borrowed cache. Always frame-owned are:
the frame header, arguments, parameter CVs (store-through, so backtraces and
observers report current values), alias-observable CVs (references, `global`,
`static`, `$this` bindings), and every slot of a function that uses dynamic
scope (`extract`, `compact`, `get_defined_vars`, variable variables,
`include`/`eval`, `func_get_args`) at the points listed below.

A value that is dead for computation but whose release is observable (a
destructor, a weak reference) stays owned until its PHP lifetime ends, in the
order Zend's live ranges and CV cleanup define.

## Canonicalization protocol

Every safepoint performs these ordered steps:

1. Record the responsible opline under the invariant above.
2. Materialize the state its boundary class reads (table below): move
   ownership of those native-owned values into their slots.
3. Publish `EG(current_execute_data)` for the frame the operation expects.
4. Keep the non-memory obligations of the activation (entry-cell activity,
   code-version references, locks, partially initialized Zend values)
   reachable from state that survives a bailout.
5. After a locally returning boundary, reload the slots, reference cells and
   heap facts the boundary may write (its declared effects), and take native
   ownership back only where the frame state says so. A register value from
   before the boundary is never reused for a slot the boundary may have
   changed.

On exception, bailout, or suspension, the corresponding edge owns state and
cleanup; no normal post-state is assumed.

## Required safepoints

| Class | Required pre-state | Normal post-state / nonlocal edge |
|---|---|---|
| Function entry | Callee identity, arguments, `$this`, return storage, parent, runtime cache, initialized CVs, and first executable opline are canonical; callee is current. | Body starts at that opline. Entry observers read the header and arguments only. |
| User call | Caller at call opline; frame header and arguments of the callee are canonical. Caller locals stay native-owned; the machine map at the call-return PC describes them for suspension, exception and deoptimization. | Callee becomes current with its entry opline. Normal return restores caller and advances; exception keeps the call opline as throw context and continues in the caller's native exception path. |
| Internal call | Caller at call opline; the internal execute-data frame and its arguments are canonical and current before observer/handler entry. Internal functions that read or write the caller's scope (`extract`, `compact`, `get_defined_vars`, `func_get_args`) additionally require the dynamic-scope materialization below. | Arguments/results are cleaned, caller becomes current, then advances. Dynamic-scope functions reload the written CVs. Exception transfers with caller call context. |
| Allocation | Responsible opcode is recorded. No materialization: the allocator does not read PHP values. | Allocation failure follows exception or bailout metadata. |
| Potential destructor | The value being released has its single owner; its release obligation is reachable outside volatile local state. | Destructors can reach only heap values and alias-observable slots: reload `EG(exception)`, `EG(current_execute_data)`, alias-observable slots and invalidated heap facts. Native-owned non-alias locals remain valid. |
| Exception edge | Throwing opline and the native exception continuation are known. | Branch to that continuation; it releases native-owned live values of the frame in Zend's cleanup order before the frame-owned cleanup runs, and unwinds exactly once. |
| Bailout helper | Non-memory obligations are reachable from the active `zend_try` catcher. Native-owned PHP values need nothing: after a bailout, as in stock PHP, dropped frames are not released and objects reach their destructors through the object store during shutdown. | No local post-state exists. Transfer is nonlocal. |
| Observer | Observed execute-data, opline, arguments, return or exception value and call flags are canonical before callback entry. | Callback may reenter, throw, or bail out; reload alias-observable slots and heap facts. An extension that inspects complete scopes (debuggers, statement handlers) selects full materialization for the functions it observes. |
| Interrupt | Current frame and responsible opline are canonical before the interrupt callback. | Callback may run PHP code, throw or bail out; reload executor globals, alias-observable slots and heap facts before advancing. Current Zend interrupt entry is shown in [`zend_execute.c`](../../../../Zend/zend_execute.c#L4314). |
| Dynamic scope | Before `extract`, `compact`, `get_defined_vars`, variable variables, `include`/`eval`, `func_get_args`: every CV of the frame is frame-owned. | Reload every CV the operation may have written; native ownership is taken back only by later definitions. |
| Generator suspend | Yield opcode, yielded key/value, send target, frame slots, pending call chain, roots, cleanup, parent policy, resume ID, and code version are persistent: ownership of every live value moves into the generator frame. | Generator is not active; persistent state owns transferred values until resume or destruction. |
| Generator resume | Persistent state, resume ID, code version, and caller chain are validated before the generator becomes current. | Reconstruct chain and roots, advance to the registered successor, reload resume-live values, then enter native code. Generator chain restoration is visible in [`zend_generators.c`](../../../../Zend/zend_generators.c#L762). |
| Fiber switch | Active VM stack, top/end, current execute data, bailout catcher, active fiber and responsible opline are captured. Native-owned values of the suspended chain stay on the fiber's own C stack, described by the machine maps at each call-return PC. | Destination state is restored atomically. While suspended, GC treats native-owned values as externally referenced (never collects them wrongly; cycles through them are collected after resume). Destroying a suspended fiber resumes it with a graceful exit, whose exception paths release native-owned values. Zend's captured fields are listed in [`zend_fibers.c`](../../../../Zend/zend_fibers.c#L105). |
| Deoptimization | Ownership of every value live at the guard moves into its frame slot, following the guard's frame-state map; the guard precedes every side effect of its operation. | Continue in the generic native version at the guard's semantic continuation through the single resume entry. |

## Roots

At a safepoint, every live refcounted PHP value is reachable through one of:

- its owner location (frame slot, or native location named by the machine
  frame-state map);
- a persistent generator suspend record;
- an established Zend executor-global or helper-owned root documented for that
  call.

The frame-state `roots` array lists rooted slot IDs. A slot marked `rooted` but
absent from that list is invalid. Conversely, a listed ID must identify a slot
in the same frame. Zend root walkers (GC of suspended generators and fibers,
live-range cleanup) see frame-owned values only; native-owned values are
externally referenced for them, which is conservative and correct.

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

Across a safepoint, registers and native stack slots may contain:

- native-owned PHP values, including refcounted ones, whose location the
  machine frame-state map of that point names;
- raw addresses whose owners remain rooted and whose validity is rechecked;
- borrowed caches of frame-owned slots, invalidated by the boundary's declared
  writes.

A native location is never the owner of a pending call, a return target, an
alias-observable value or a continuation. No callee-saved-register convention
extends the public Zend ABI: native-to-native calls keep the frame header and
arguments in Zend layout.

## Verification rule

The compiler enumerates each boundary class in metadata. Publication verifies
that the state the class reads is published before, that the state it may
write is reloaded after every locally returning edge, and that every exception,
bailout and suspension edge has its continuation and obligations. An
unclassified helper is treated as reading and writing every slot of its frame,
capable of allocation, exception, destructor reentry, observation, interrupt
interaction and bailout (full materialization before, full reload after) until
a narrower reviewed contract classifies it.

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
- deoptimization is rejected when any logical parent, root, cleanup
  obligation, resume ID, or code-version identity is absent;
- native-owned values: exception unwinding, destructor reentry through an
  alias, dynamic-scope functions, observers, interrupts, and fiber
  suspension/destruction release every value once and report the same
  backtraces and parameter values as stock PHP.
