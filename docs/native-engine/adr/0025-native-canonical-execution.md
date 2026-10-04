# ADR 0025: Native state is canonical; Zend frames are observed, not mirrored

## Status

Accepted. Linux x64. The goal is maximum execution performance for real PHP
applications such as WordPress, measured on warm requests.

## Context

Keeping the `zend_execute_data` frame as the only authoritative copy of PHP
state forces native code to write every CV and TMP to its slot, to pass
encoded operands to helpers that decode them against the frame, to build a
universal activation for every call, and to forgo specialization from runtime
types. Most observers read much less than the complete frame:

| Observer | Reads |
|---|---|
| Exception propagation and catch | Nothing outside the throwing function: a native callee returns `ZEND_NATIVE_EXCEPTION` and the caller's own native code, which still holds its values, runs its cleanup and catch dispatch. |
| Warnings, notices, `error_get_last` | Current function and line of the active frame. |
| `debug_backtrace`, exception traces | For every frame in the chain: function, `$this`/scope, line, and arguments (current parameter values). |
| Observers (`zend_observer_fcall_*`) | Function, arguments, return value of the observed frame. |
| Interrupts, timeouts, signals | The line of the polling frame; handlers cannot read the frame's locals. |
| GC (`gc_collect_cycles`) | Nothing on the stack: PHP's collector works from refcounts and its root buffer, so a value held only in a register or native stack slot is simply externally referenced. |
| `compact`, `extract`, `get_defined_vars`, `$$`, `include`/`eval`, `func_get_args` | All CVs (or all arguments) of the current frame. |
| Generator and fiber suspension | The complete frame. |
| Deoptimization (this ADR) | The complete frame at the guard. |

Reconstruction is exact and compiler-verified, never best effort: native code
only stops materializing state that no observer reads.

## Decision

### 1. Canonical native state with an exact observation contract

Native code owns the values of its CVs and TMPs in SSA values, registers and
native stack slots. The Zend frame stays the public representation for
extensions and runtime services, with this split:

- **Always current (eager):** the frame header — `func`, `prev_execute_data`,
  `This` and call info, `return_value`, argument count, `run_time_cache` — and
  the argument and parameter slots. A parameter CV is store-through, because
  backtraces and observers report current parameter values. `EX(opline)` is
  stored before every transition into C code that can observe a line (helpers,
  internal calls, polls), as today.
- **On demand:** all other CVs and every TMP/VAR. They are written to their
  slots only where the table above requires it:
  - before dynamic-scope operations (`compact`, `extract`,
    `get_defined_vars`, variable variables, `include`/`eval`,
    `func_get_args`), which the compiler knows statically;
  - at generator/fiber suspension;
  - at deoptimization;
  - before a helper whose declared effects read or write that slot.

Every instruction keeps its MIR `frame_state_id`. The backend emits, per
observation point, a frame-state map naming the machine location of every
live PHP value, root and cleanup obligation (register, native stack slot,
constant, or frame slot). The maps are verified before publication.
Materialization, deoptimization and
later lazy frame construction all read the same map.

### 2. Helpers become semantic primitives

New and rewritten helpers take values, addresses and flags, not encoded opcode
operands, and do not reread `zend_op` (compare `zend_jit_helpers.c`). They
declare their effects in MIR — may allocate, throw, reenter, read or write a
frame slot — so that step 1 materializes only what a helper really reads.
Operations that release values in generated code call a single release
sequence that runs the GC root check (`gc_check_possible_root`) and the
destructor in the runtime only for the last owner or a possible root.

### 3. Native calling convention between compiled functions

A call from compiled PHP code to compiled PHP code with a resolved target
uses a native calling convention:

- the call site checks its target inline (function pointer, class and
  method, closure code) against a per-site cache;
- on a hit it bumps the VM stack, writes the callee frame header and the
  arguments directly into the callee's parameter slots, and calls the native
  entry with the frame and argument values in registers;
- the result comes back in registers; the caller releases the frame.

The universal activation, `zend_native_call_resolve_user`,
`zend_native_call_invoke_user` and per-argument
`zend_native_call_set_explicit_argument` remain as the cold path for a cache
miss and for the forms the fast path does not cover: named, variadic and
by-reference arguments, magic `__call`, trampolines, observers.

Two WordPress forms are fast-path forms, not cold ones:

- `call_user_func_array` / `SEND_ARRAY` with packed positional arguments and
  no named or by-reference parameter: the argument array is copied directly
  into the callee's parameter slots;
- hook dispatch sites with changing targets: an already resolved target
  (entry cell, valid native binding) is invoked without re-running the
  general resolution and activation machine; the per-site cache is only the
  first level.

The call fast path uses the same ownership, observation and suspension rules
as section 1 (a caller's native-owned locals are described by the map at the
call-return PC); it is developed in parallel with, not independently of, the
frame-state and cleanup work.

The callee frame header stays eager (section 1), so extensions and runtime
services keep a walkable `EX` chain. Building headers lazily as well, from the
native stack, is only done when inlining requires it (section 5) or when
measurements show that the header stores matter.

### 4. Speculation with deoptimization into generic native code

- **Type feedback:** generic native code records, per site, the observed
  operand types, array shapes (packed, hash), receiver classes and call
  targets in a side array next to the function's `run_time_cache`. Recording
  is a store on the existing slow paths, not on the fast path.
- **Specialization:** a function that is called often is recompiled with the
  recorded facts as guarded assumptions. Values become `i64`/`f64`/pointer SSA
  values, a property access with a known class becomes a load at a fixed
  offset, and a call with a known target becomes a direct call or is inlined.
- **Deoptimization:** a failed guard transfers the state from its frame-state
  map into the Zend frame and continues in the generic native version of the
  same function at the same source position. The generic version is compiled
  with resume entries for the deoptimization points (the resume ABI already
  reserves this, see `semantics/frames/resume-abi.md`). Execution never
  resumes in the VM.
- **Invalidation:** code that depends on a class layout, a function or
  constant binding, or a declaration epoch is registered with that
  dependency. Changing it retires the code through the existing entry-cell
  generations, and active frames deoptimize at their next guard or return.
- A site that deoptimizes repeatedly is recompiled without that assumption.

### 5. Inlining with guards

Small callees (getters, wrappers, hook plumbing) are inlined at call sites
whose target is known or speculated, guarded by class and target checks.
Inlined frames have no Zend frame of their own. Their state is described by
the frame-state `parent_id` chain, and a Zend frame is built lazily from it
when an observer needs one: backtraces, exception traces, warnings, and
observers. This is exact reconstruction under the section 1 contract.

### 6. Unchanged rules

- No production VM fallback, no Zend VM opcode handler reuse, no Zend JIT IR,
  no second register allocator.
- PHP-observable behaviour is identical at every observation point:
  refcount, destructor and GC-root ordering, warnings and their order,
  exceptions, observers, backtraces, `func_get_args`, generators and fibers.
- Public Zend headers stay C-compatible. Extensions see a valid `EX` chain at
  every transition into C code.

## Delivery

The acceptance measure is the total per warm request against stock PHP. The
generic native code must be competitive on its own; speculation and inlining
add to it. Existing mechanisms are extended rather than duplicated: frame
states, entry cells, generations, the resume contract, and
`generate_guarded_direct_exit` for guard exits. Each step is measured on the
same warm WordPress request (cycles, instructions, L1i and iTLB misses, hot
code size, compile time) with unchanged page output and the full PHPT tier.

1. **Frame states as machine maps; lazy CV/TMP slots.** The backend emits
   verified machine frame-state maps. Non-parameter CVs and TMPs are written
   to their slots only at the materialization points of section 1, under the
   single-owner rule of the safepoint contract (a native-owned value's slot
   holds `IS_UNDEF`), with native exception cleanup for native-owned values.
   Helpers declare the slots they read and write.
2. **Helper primitives.** Helpers take values, with the shared release
   sequence, in the order of the profile (`ASSIGN_DIM`, `ASSIGN`,
   `FETCH_DIM`, object read and write, conditional branches, array
   construction, iteration). Array probes handle non-interned string keys by
   hash, length and content.
3. **Native calls.** Section 3 for user functions, methods, static methods,
   closures, `new`, packed `call_user_func_array` and changing hook targets.
4. **Type feedback, specialization, deoptimization.** Section 4, starting
   with integer and float operations, packed arrays and property offsets.
   Every deoptimization point has a PHPT that forces it.
5. **Guarded inlining and lazy frames for inlined callees.** Section 5, with
   backtraces, traces, warnings and observers identical to stock inside
   inlined code.

Steps 1 and 2 go together, since a helper that reads frame slots forces their
materialization. Step 3 is independent. Step 4 needs the frame-state maps of
step 1, step 5 needs steps 3 and 4.

## Consequences

- The Zend frame is no longer the only copy of a function's state. Every
  change to generated code, helpers or observation points must keep the
  frame-state maps complete; a missing entry is a correctness bug.
- Tests must force every observation class: warnings, exceptions and traces,
  backtraces with modified parameters, observers, dynamic-scope functions,
  generators, fibers, and each guard/transfer class of deoptimization in the
  relevant combinations with aliasing, pending calls, side effects,
  suspension and register pressure (not one PHPT per dynamic deopt instance).
  Compare against stock PHP as the oracle.
- Compile time grows with specialization and inlining. Compilation of the
  generic version stays single-pass TPDE; specialized versions are compiled
  only for hot functions.
- Extensions keep a valid `EX` chain at every C transition because the frame
  header stays eager.

## Alternatives

- **Keeping every value in the Zend frame and adding inline forms per
  opcode** cannot remove the synchronization cost of two representations.
- **Fully lazy frames, without an eager header,** make every C transition
  depend on native stack walking; they come with inlining (step 5).
- **Deoptimization into the VM** keeps the VM in the production path and is
  excluded.
- **Guards only at function entry** cannot specialize values loaded inside a
  function (array elements, properties, call results); entry variants remain
  one form of specialization.

## Verification impact

- Frame-state maps are verified before code publication.
- PHPTs cover each observation class of the context table in plain,
  specialized and inlined code, under low and high register pressure.
- Deoptimization tests force every guard kind and compare the output with
  stock PHP.
- The WordPress page check (`/`, `/?s=hello`, `/sample-page/`) and the full
  PHPT tier (debug, ASan, UBSan with the known baseline) run before every
  push.
