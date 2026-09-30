# ADR 0025: Native state is canonical; Zend frames are observed, not mirrored

## Status

Accepted on 2026-09-30. Linux x64. Supersedes in part ADR 0004 (rejection of
lazy frame reconstruction and of a separate native frame ABI), ADR 0009
(every live value canonical in the Zend frame at every safepoint) and ADR 0024
("there is no deoptimization"). The goal is maximum execution performance for
real PHP applications such as WordPress.

## Context

A warm WordPress front-page request executes 580M user instructions natively
against 258M in stock PHP with OPcache. The shared runtime (hash tables,
allocator, strings, libc, extensions) costs about the same in both (159M
against 137M). The difference is the layer that replaces the interpreter:

| Native layer | Instructions per request | Stock equivalent |
|---|---|---|
| Generated code | 170.5M | VM handlers: 115.1M in total |
| Opcode-shaped value and object helpers | 126.3M | |
| Call protocol (resolve, invoke, activation, frames, arguments) | 112.8M | |

The generated code alone costs more than the whole stock interpreter. (The
5.9M stock instructions in generated code are PCRE JIT code for regular
expressions; stock runs without the PHP JIT. The native figure contains the
same share.) Altogether the native execution layer, including the remaining
native runtime, costs 420.5M against 121.0M in stock: 93 % of the extra
instructions are in our own layer, 22M in the shared runtime. An empty
integer loop takes 48 instructions per iteration against 54 in the VM. A method
call takes 703 instructions against 285.

The cause is the execution model of ADR 0004 and ADR 0009. The
`zend_execute_data` frame with its zval slots is the only authoritative copy
of PHP state, so native code keeps two representations in sync:

- every CV and TMP write goes to its frame slot;
- helpers receive encoded opcode operands and decode them against the frame
  (`zend_native_value_init_explicit_operation`);
- every call builds a universal activation (224 bytes, zeroed store by store)
  before `zend_native_call_resolve_user` and `zend_native_call_invoke_user`;
- guards branch through materialized decision registers;
- nothing is specialized from runtime types, since there is no way to leave a
  specialized body.

Opcode-by-opcode snippets (commits d9a192011b6 through 709252f905a) removed
about 13 % of the instructions; each further snippet is worth 1–3 %. Reaching
and passing stock PHP needs the model itself to change.

### What actually observes a frame

The survey behind this ADR (Zend/Native as of 709252f905a) shows that most
observers read much less than the complete frame:

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

ADR 0004 rejected lazy reconstruction because "asynchronous and exceptional
observation points cannot tolerate missing state". This ADR keeps that
requirement: reconstruction is exact and compiler-verified, never best effort.
It only stops materializing state that no observer reads.

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
constant, or frame slot). The maps are verified before publication, as ADR
0009 requires for frame states today. Materialization, deoptimization and
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

The phase targets below are intermediate targets, not a budget for parity:
with helper and call costs just below their targets and the shared runtime
unchanged, 270M instructions would already be spent before any application
code. The acceptance measure is the total per warm request against stock.

Order (review of 2026-09-30): first the contract documents and the snippet
regeneration build; then phases 1 and 2 together (native value handling for
typed and untyped, boxed SSA values, targeted materialization with its
reverse direction, native cleanup, value-based primitives); phase 3 in
parallel with the shared state contract; then bounded speculation with
deoptimization, then bounded inlining. The generic native baseline must be
competitive on its own; speculation and inlining add to it and do not
replace it. The existing mechanisms are extended rather than duplicated:
frame states, entry cells, generations, the resume contract, and
`generate_guarded_direct_exit` for guard exits instead of materialized
decision registers. Code size is analysed (executed code, cold code, stubs,
metadata, mapping) before the publisher is changed; W^X stays.

Each phase is measured on the same warm WordPress request (retired
instructions, cycles, L1i and iTLB misses, hot code size, cold compile time),
with unchanged page output, and must pass the full PHPT tier. The loop, call
and property targets use the micro-benchmark from this ADR's context.

1. **Frame states as machine maps; lazy CV/TMP slots.** The backend emits
   verified machine frame-state maps. Non-parameter CVs and TMPs stop being
   store-through outside the materialization points of section 1, with the
   single-owner rule of the safepoint contract (a native-owned value's slot
   holds `IS_UNDEF`) and native exception cleanup for native-owned values.
   Helpers declare the slots they read and write. The empty integer loop is a
   focused regression target (48 instructions per iteration now), not the
   proof of the phase; generated code on WordPress falls clearly below 170M.
2. **Helper primitives.** Rewrite the helpers in instruction order of the
   profile (`ASSIGN_DIM`, `ASSIGN`, `FETCH_DIM`, object read and write,
   conditional branches, array construction, iteration) to take values, with
   the shared release sequence. Array probes handle non-interned string keys
   by hash, length and content (numeric strings and real collisions stay
   correct) instead of leaving them to the operand-decoding helper. Target:
   helper instructions per request from 126M to under 60M.
3. **Native calls.** Section 3 for user functions, methods, static methods,
   closures, `new`, packed `call_user_func_array` and changing hook targets.
   Target: a method call from 703 to under 150 instructions, the same order
   for a hook site with changing targets; call protocol on WordPress from 113M
   to under 40M.
4. **Type feedback, specialization, deoptimization.** Section 4, starting
   with integer and float operations, packed arrays and property offsets.
   Target: loops and property-heavy code run on unboxed values; every
   deoptimization point has a PHPT that forces it.
5. **Guarded inlining and lazy frames for inlined callees.** Section 5.
   Target: fewer calls executed on WordPress, with backtraces, traces,
   warnings and observers identical to stock inside inlined code.

Phases 1 and 2 depend on each other, since a helper that reads frame slots
forces their materialization. Phase 2 therefore goes before or together with
the lazy-slot part of phase 1. Phase 3 is independent of both. Phase 4 needs
the frame-state maps from phase 1. Phase 5 needs phases 3 and 4.

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
- ADR 0004's objection to a separate frame ABI remains valid for extensions.
  It is answered by keeping the frame header eager and the `EX` chain valid
  at every C transition, not by giving it up.

## Alternatives

- **Keep the ADR 0009 model and add snippets per opcode.** Measured at 1–3 %
  per step; it cannot remove the synchronization cost that makes the native
  layer 3.5 times as expensive as the interpreter.
- **Fully lazy frames from the start**, with no eager header. This makes every
  C transition depend on native stack walking before any gain from the
  cheaper parts is measured. It is deferred to phase 5, where inlining makes
  it necessary.
- **Deoptimization into the VM.** Simpler, but it contradicts the engine
  replacement goal and keeps the VM in the production path. Rejected.
- **Speculation without deoptimization, with guards only at function entry.**
  Covers argument types, but not values loaded inside the function (array
  elements, properties, call results), which dominate WordPress. Rejected as
  the end state; entry variants remain as one form of specialization.

## Verification impact

- Frame-state maps are verified before code publication.
- PHPTs cover each observation class of the context table in plain,
  specialized and inlined code, under low and high register pressure.
- Deoptimization tests force every guard kind and compare the output with
  stock PHP.
- The WordPress page check (`/`, `/?s=hello`, `/sample-page/`) and the full
  PHPT tier (debug, ASan, UBSan with the known baseline) run before every
  push.
