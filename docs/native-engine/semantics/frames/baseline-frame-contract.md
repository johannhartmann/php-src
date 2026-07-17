# Baseline frame contract

This document is normative for native baseline execution. “Canonical” means
that existing Zend code can inspect or operate on the state without consulting
target registers or backend-private layout knowledge.

## Physical model

Every active native PHP call is represented by the existing
`zend_execute_data` header and its contiguous VM-stack slots. Zend defines the
header fields in [`zend_compile.h`](../../../../Zend/zend_compile.h#L645), the
slot address calculation in
[`zend_compile.h`](../../../../Zend/zend_compile.h#L714), and the concrete slot
ordering in [`zend_execute.c`](../../../../Zend/zend_execute.c#L4349).

Native-private metadata is referenced by an immutable code version or an
out-of-line runtime record. It does not change `zend_execute_data`, `zval`, call
flags, slot offsets, or any exported C signature.

## `zend_execute_data` components

| Component | Canonical native requirement | Ownership and observability |
|---|---|---|
| `opline` | Points at the current user opcode under the rules in the [safepoint contract](safepoint-contract.md). Internal functions have no user `op_array`; their caller retains the responsible call opline. | Observable by errors, exceptions, observers, interrupts, backtraces, and reentry. |
| `call` | Names the chain of initialized but unfinished call frames owned by this frame, or `NULL`. | Each pending call and initialized argument is a root and has an unwind cleanup recipe. |
| `return_value` | Names caller-owned result storage or is `NULL` when the result is unused. The storage is `UNDEF` until a result is produced. | The caller owns storage; the callee owns producing or leaving it `UNDEF` on a failing edge. |
| `func` | Names the exact `zend_function`; for user code it also identifies the `op_array`. | Immutable for the frame lifetime and visible to extensions, observers, and backtraces. |
| `This` | Preserves the canonical `$this` zval plus call flags and argument count. | `$this` is a root when present. Release follows existing call flags exactly. |
| `prev_execute_data` | Names the observable parent frame or `NULL` at the top. | Updated before publishing `EG(current_execute_data)`; suspended frames reconstruct the same logical chain on resume. |
| `symbol_table` | Names an attached symbol table or is `NULL`. CV/symbol-table aliasing is synchronized before observation. | Frame-owned under the existing call flag and rooted until detached or frame cleanup. |
| `run_time_cache` | Names the cache belonging to the exact `op_array` and code lifetime. | It is metadata, not a replacement for PHP-value roots. |
| `extra_named_params` | Names canonical extra named arguments or is `NULL`. | Entries are roots and are released by the call-frame cleanup selected by call flags. |

User-frame initialization already establishes opline, return storage, CVs,
runtime cache, and `EG(current_execute_data)` in
[`zend_execute.c`](../../../../Zend/zend_execute.c#L4437); native entry preserves
that ordering.

## VM-stack slot classes

The frame begins with the reserved execute-data slots. Argument slots occupy
the first variable positions, followed by remaining CVs, then VAR/TMP slots;
copied extra positional arguments follow the declared variable/temporary area.

| Slot class | Representation | Initial owner | Canonical lifecycle |
|---|---|---|---|
| Declared argument | `zval` in `ZEND_CALL_ARG`/CV position | Callee frame after call publication | Initialized before callee entry; rooted while live; destroyed or transferred using parameter/call semantics. |
| Compiled variable (CV) | `zval` in `EX_VAR_NUM` | Callee frame | Non-argument CVs begin `UNDEF`; every defined CV is rooted and synchronized with an attached symbol table. |
| VAR | `zval` in op-array temporary area | Frame or pending operation | Materialized according to opcode live ranges; cleanup follows the exact producing operation and exception edge. |
| TMP | `zval` in op-array temporary area | Frame | May be `UNDEF` outside its live range; a live refcounted TMP is rooted and has one cleanup obligation. |
| Extra positional argument | copied `zval` after CV/VAR/TMP area | Callee frame | Rooted and released when `ZEND_CALL_FREE_EXTRA_ARGS` requires it. |
| Extra named argument | entry in `extra_named_params` | Callee call frame | Rooted and released by named-argument call cleanup. |
| Return storage | caller slot or temporary local result | Caller | Callee writes a canonical zval; ownership transfers to caller on normal return. Unused results are destroyed before the next boundary. |
| `$this` | `This` field | Call frame | Borrow/addref/release behavior is encoded by call flags; it stays rooted for the frame lifetime. |
| Pending call | linked `zend_execute_data` through `call` | Owning frame | Function identity, arguments, flags, and named parameters are canonical; exception cleanup can traverse the chain. |

The frame-state schema uses a stable `slot_id` rather than a byte address. Its
`kind`, `index`, `representation`, `materialization`, `ownership`, `rooted`, and
`cleanup_required` fields describe these rows. Two slots in one frame cannot
share an ID.

## Observable boundary state

Before any call, allocation, possible destructor, exception transfer, bailout
helper, observer, interrupt, suspension, resumption, or future deoptimization:

1. `EG(current_execute_data)` names the frame the Zend operation expects.
2. Every published frame has final `func`, `This`, argument count,
   `prev_execute_data`, return storage, call chain, and opline state.
3. Every live PHP value is in a canonical zval reachable through a frame,
   persistent suspend record, executor-global root, or helper-owned argument.
4. The `roots` list names every live rooted slot. A register copy does not add a
   root and cannot outlive mutation or reentry of the canonical zval.
5. Every slot with `cleanup_required: true` has exactly one pending or
   transferred cleanup obligation. The obligation identifies the state that
   remains reachable if local control does not return.
6. Return and exception continuations identify a frame and opline. The bailout
   continuation is explicitly `nonlocal_bailout` and has no local target.

Unobservable straight-line native code may keep non-refcounted scalars in
registers. A live PHP value may also be cached in a register only while its
canonical zval remains authoritative and rooted; reentry invalidates that
cache.

## Calls and returns

### Call publication

The caller first records the call opcode in its `opline`, constructs the
callee/pending frame and arguments, links `prev_execute_data`, and establishes
return storage. Only then may it publish the callee through
`EG(current_execute_data)` or invoke an observer/helper. The current VM follows
this order for internal and user calls in
[`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L4127).

At user-function entry, the callee opline is the first opcode that will execute;
skipped receive opcodes are reflected by that index. At internal-function
entry, the internal frame is fully materialized but carries no user-opline
index; its caller frame retains the call opcode as the responsible position.

### Normal return

Before the callee becomes unobservable, it writes or transfers the result,
runs the return observer with canonical callee state, completes callee-owned
cleanup, restores `EG(current_execute_data)` to the parent, and advances the
parent opline to the call successor. Cleanup that may invoke a destructor is
itself a safepoint and obeys the reentry rules.

### Exceptional return

The throwing frame retains the responsible opline separately from the
exception-dispatch sentinel. Pending calls, live temporaries, arguments, and
frame-owned values are unwound from explicit cleanup metadata. The current VM
uses the saved throwing opline to select try/catch/finally and clean unfinished
calls in [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8222).

## Parent frames and future inlining

`parent_frame_id` describes the logical `prev_execute_data` chain. IDs are
unique inside a state set, every non-null parent exists, and the graph is
acyclic. W01 represents physical baseline frames. A future optimizer may add
logical inlined frames to metadata, but before an observable boundary it must
materialize a Zend-compatible acyclic chain with the same arguments, locals,
oplines, roots, and cleanup. This allowance does not define a separate
optimizer ABI.

## Publication invariant

A code version is publishable only when every native boundary has a complete
frame-state record conforming to
[`frame-state.schema.json`](frame-state.schema.json), every resume ID resolves
within that immutable version, and root/cleanup verification succeeds. Failure
rejects publication of the execution unit.
