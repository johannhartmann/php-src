# ADR 0025: Native state is canonical; Zend frames are observed, not mirrored

## Status

Accepted. The goal is maximum execution performance for real PHP applications
such as WordPress, measured on warm requests. Linux x86-64 implements the
model. The Darwin arm64 back-end has no lazy CV stores, deoptimization or
speculation (ADR 0006).

## Context

If the `zend_execute_data` frame is the only authoritative copy of PHP state,
native code must write every CV and TMP to its slot and pass encoded operands
to helpers. It must also build a universal activation for every call and
cannot specialize from runtime types. Most observers read much less than the
complete frame:

| Observer | Reads |
|---|---|
| Exception propagation and catch | Nothing outside the throwing function: a native callee returns `ZEND_NATIVE_EXCEPTION`, and the caller's own native code, which still holds its values, runs its cleanup and catch dispatch. |
| Warnings, notices, `error_get_last` | Current function and line of the active frame. |
| `debug_backtrace`, exception traces | Per frame in the chain: function, `$this`/scope, line, arguments (current parameter values). |
| Observers (`zend_observer_fcall_*`) | Function, arguments and return value of the observed frame. |
| Interrupts, timeouts, signals | Line of the polling frame; handlers cannot read its locals. |
| GC (`gc_collect_cycles`) | Nothing on the stack: the collector works from refcounts and its root buffer, so a value held only in a register or native stack slot is externally referenced. |
| `compact`, `extract`, `get_defined_vars`, `$$`, `include`/`eval`, `func_get_args` | All CVs (or all arguments) of the current frame. |
| Generator and fiber suspension | The complete frame. |
| Deoptimization (section 4) | The complete frame at the guard. |

Reconstruction is exact and compiler-verified, never best effort. Native code
stops materializing only the state that no observer reads.

## Decision

### 1. Canonical native state with an exact observation contract

Native code owns the values of its CVs and TMPs in SSA values, registers and
native stack slots. The Zend frame stays the public representation for
extensions and runtime services.

- **Always current (eager):** the frame header (`func`,
  `prev_execute_data`, `This` and call info, `return_value`, argument count,
  `run_time_cache`) and the argument and parameter slots. A parameter CV is
  store-through, because backtraces and observers report current parameter
  values. `EX(opline)` is stored before every transition into C code that
  can observe a line (helpers, internal calls, polls).
- **On demand:** all other CVs and every TMP and VAR. They are written to
  their slots only where the table above requires it:
  - before dynamic-scope operations (`compact`, `extract`,
    `get_defined_vars`, variable variables, `include`/`eval`,
    `func_get_args`), which the compiler knows statically;
  - at generator or fiber suspension and at deoptimization;
  - before a helper whose declared effects read or write that slot.
- **Single owner:** while native code owns a value, its slot holds `IS_UNDEF`
  or a non-refcounted value, never a stale pointer
  ([safepoint contract](../semantics/frames/safepoint-contract.md)). Native
  exception cleanup releases native-owned values. Values that may be aliased
  keep their slot authoritative (ADR 0024).

Every instruction keeps its MIR `frame_state_id`. Per observation point, the
backend emits a frame-state map. The map names the machine location of every
live PHP value, root and cleanup obligation: register, native stack slot,
constant or frame slot. The maps are verified before publication.
Materialization, deoptimization and lazy frame construction read the same
map. A missing entry is a correctness bug.

### 2. Helpers are semantic primitives

Helpers take values, addresses and flags, not encoded opcode operands, and do
not reread `zend_op`. Each declares its effects in the helper table
(`zend_native_runtime_helpers[]`, `ZEND_NATIVE_RUNTIME_EFFECT_*`): allocate,
throw, bail out, reenter, frame read or write, and others. Section 1 then
materializes only what a helper really reads. Generated code releases values
through one release sequence. That sequence calls into the runtime for the
destructor or `gc_check_possible_root()` only for the last owner or a
possible root.

### 3. Native calling convention between compiled functions

A call from compiled PHP code to compiled PHP code with a resolved target
uses the native convention:

- The site checks its target inline (function, class and method, closure
  code) against its per-site cache (`zend_native_user_call_site_header`).
- On a hit, the site bumps the VM stack, writes the callee frame header and
  the arguments directly into the callee's parameter slots, and calls the
  target's native call entry. The caller frame, execution context, site
  descriptor and result offset travel in registers.
- The native call entry (`zend_native_code_fast_call_entry()`) is frameless
  code placed directly before the target's Zend entry. It checks the pushed
  frame against facts the compiler froze (fewest and most arguments, no named
  or undefined arguments, type bits of typed parameters). It initializes the
  frame as `i_init_func_execute_data()` and the target's `RECV`, `RECV_INIT`
  and `RECV_VARIADIC` do: literal defaults are immediates, and
  `zend_native_call_receive_variadic()` collects an untyped by-value variadic
  parameter. It runs the body and leaves the frame inline for a plain
  return. `$this` and closure releases, extra arguments and failures leave
  through the shared `zend_native_call_fast_leave()`, which keeps every entry
  small.
- A site whose publication proved the argument count and untyped parameters
  enters past the checks (`zend_native_code_exact_call_entry()`).
- The result is written to the caller's result slot, which the site's
  consumers read.

Arguments and the result travel through frame slots because generic code
keeps its values there. Register passing pays only where both sides keep
values in registers. Typed component bodies (ADR 0024) pass arguments and
results in registers.

The universal protocol is the cold path for cache misses and for the forms
the fast path does not cover. It consists of the universal activation,
`zend_native_call_resolve_user()` and per-argument
`zend_native_call_set_explicit_argument()`. The uncovered forms are named and
by-reference arguments, typed or by-reference variadic parameters, magic
`__call`, trampolines and observers. Any frame a native call entry rejects
continues, untouched, in `zend_native_call_fast_do()`.

Two more forms are fast-path forms:

- `call_user_func_array` / `SEND_ARRAY` with packed positional arguments
  (also as `array_slice()` of them) to a target without named or by-reference
  parameters. The array is copied directly into the callee's parameter slots.
- Sites with changing targets (`$f(...)`, `call_user_func()`, variable
  methods). `zend_native_call_fast_dynamic_init()` finds an already resolved
  target (entry cell, valid native binding) by a stable identity, without
  the general resolution and activation. The per-site cache is only the
  first level.

The callee frame header stays eager, so extensions and runtime services keep
a walkable `EX` chain. Headers are built lazily from the native stack only for
inlined callees (section 5). The call fast path follows the ownership,
observation and suspension rules of section 1: the map at the call-return PC
describes a caller's native-owned locals.

**State that outlives the request.**

- **Call sites.** A site outlives the request when its target is an
  immutable user function or a persistent internal function that no binding
  of the request selects: a method (the guard compares the receiver's
  class), `new self`/`parent`/`static`, `static::` (the called scope), and
  `self::` or `parent::` from an immutable caller scope (closures and trait
  methods compare the caller's scope with the site's key). It holds its
  target until the target's cell is reset or gets other code, which runs
  `zend_native_call_persistent_sites_invalidate()`, and takes the callee's
  run-time cache of the current request, creating it on the first call.
  Function names, `new C` and `C::m()` bind per request; their sites re-arm
  once per request (`zend_native_call_fast_rearm()`).
- **Property resolutions.** What a property operation with a literal name
  caches (class, slot offset, typed property info) depends only on the
  class, the name and the op array's scope. Take an immutable op array that
  is no closure and a declared property of an immutable class. Request
  shutdown keeps the resolution in a per-process template of the op array
  (`zend_native_property_cache_learn()`). The next request's run-time cache
  starts from it (`zend_native_property_cache_prime()`). Property values and
  dynamic and hooked properties stay per request. A new native generation (an
  OPcache restart, or a replaced script that declares classes) drops the
  templates.
- **Code lookups.** The universal protocol and entries from C find a
  function's cell in the reentry cache, which keeps persistent cells under
  the persistent epoch. A cached include finds its entry cell by opcodes in a
  process-local include cache, once its persistent generation is settled:
  `zend_native_compiler_end_request()` has nothing left to drop. Without
  threads only a request boundary retires a persistent generation, so it
  needs no per-request lease. OPcache records the scripts it discards for
  good in a small ring, so a process retires exactly their generations.

### 4. Speculation with deoptimization into generic native code

- **Type feedback.** Generic native code records, per site, the observed
  operand types, array shapes (packed, hash), receiver classes and call
  targets in a side array next to the function's `run_time_cache`. Recording
  is a store on the existing slow paths, never on the fast path.
- **Specialization.** A function that is called often is recompiled with the
  recorded facts as guarded assumptions: values become `i64`, `f64` or
  pointer SSA values, a property access with a known class becomes a load at
  a fixed offset, and a call with a known target becomes a direct call or is
  inlined.
- **Deoptimization.** A failed guard transfers the state from its frame-state
  map into the Zend frame. Execution continues in the generic native version
  of the same function at the same source position, never in the VM. The
  generic version has resume entries for its deoptimization points
  ([resume ABI](../semantics/frames/resume-abi.md)).
  - **Resume IDs and landings.** A resume ID names a guarded operation
    outside every pending call. Its landing is the start of the operation's
    cold block, where the generic operation begins. An entry whose context
    has `deopt_resume` set skips the entry variant dispatch and the
    initialization of temporaries. A gateway after the frame load dispatches
    on `EX(opline)` to the landing's stub. The stub loads every value of the
    landing's map from its canonical frame slot into the register or stack
    slot TPDE holds it in at the block start, then falls into the block.
  - **Landing maps.** A landing's map is the machine liveness at the block
    start (TPDE values, including unboxed arguments and hoisted payloads),
    restricted to values defined on every path into the block. A map value
    must be a MIR value its slot holds or a payload loaded from its slot;
    otherwise the landing is dropped. The transferring slow path first
    stores the map's register scalars (lazily written CVs, typed values) to
    their slots. Pointers stay with their slots.
  - **Specialized version** (opt-in: `ZEND_NATIVE_SPECULATE=1`, Linux x64).
    It is the function itself; the compiler adds a hidden generic copy with
    landings at every guarded operation. The copy keeps no long induction
    in a register only, so that every landing reloads its frame state from
    the slots. The specialized version assumes:
    - ADD, SUB, MUL, compound assignments, `++` and `--` of longs yield
      longs, exiting where they would overflow;
    - an element read yields the type the script's own code stores into
      that array (`zend_native_element_guess()`: stores, two array levels
      deep, followed through assignments, parameters and return values of
      the script's functions). The read checks the element's type and exits
      from its cold block, before the read, for another type, a reference,
      a missing key or another container;
    - a main script's globals hold at entry the scalar types they hold
      while it compiles, behind entry guards, as far as no boundary that
      may run user code separates a global's definition from a use. A
      global's register value is published to its slot at the first
      boundary after its definition and never again: a callee may have
      changed the global since.

    The types narrowed by these assumptions are inferred again from nothing
    up (a loop's phi would otherwise keep the type its back edge had before).
    An exit stores the register scalars the continuation carries, calls
    `zend_native_deopt_transfer()` with the operation's source position and
    returns the copy's status. An interrupt poll exits too: the copy handles
    the interrupt at its next poll, and the specialized loop calls no
    handler. Other guarded operations keep their cold paths. Each exit
    counts in the function's entry cell; from
    `ZEND_NATIVE_SPECULATION_EXIT_LIMIT` (16) exits on, its entry runs the
    copy. Generators are not specialized.
  - **Stress mode.** Debug builds with `ZEND_NATIVE_DEOPT_STRESS=<period>`
    give every non-generator function resume IDs. Every period-th cold path
    of a guarded operation transfers to a new activation of the same
    function (`zend_native_deopt_stress_reenter()`), which disarms its own
    points.
- **Recompilation (tier 2)**, off unless `ZEND_NATIVE_TIER2_THRESHOLD` is
  positive.
  - The Zend entry of a named, non-generator user function counts its calls
    down in its entry cell; an integer variant counts in its function's
    cell.
  - At zero the entry calls `zend_native_tier2_note()`. It recompiles at once
    through `zend_native_executor_recompile_tier2()` in the generation that
    owns the cell. The function is queued for request shutdown only when an
    exception is pending.
  - Every published member of the function's image gets a copy. The copies,
    their variants and their generic copies call each other
    component-locally. Each member's entry cell then publishes its copy, so
    later calls and re-armed sites enter it. Frames still running the first
    code finish there. Tier-2 code stays local to the process and is never
    serialized.
  - The last 64 counted calls record their argument types per parameter in
    the cell. A required, untyped by-value parameter whose arguments all had
    one type (`int|float` and `bool` count as one) is declared with that
    type in the copy. The copy's entry guards these parameters and runs the
    tier-1 code for other arguments. A function with an integer variant keeps
    the variant instead.
  - Tier 2 does not speculate on result types: the generic fast paths already
    check the types such a speculation proves.
- **Invalidation.** Code that depends on a class layout, a function or
  constant binding, or a declaration epoch is registered with that
  dependency. Changing the dependency retires the code through the entry-cell
  generations, and active frames deoptimize at their next guard or return.
- **Repeated deoptimization.** Code that deoptimizes repeatedly stops
  speculating: a specialized version retires to its generic copy (see
  above); recompiling it without the failed assumption is not done yet.

### 5. Inlining with guards

Small callees (getters, wrappers, hook plumbing) are inlined at call sites
whose target is known or speculated, guarded by class and target checks.
Inlined frames have no Zend frame of their own. The frame-state `parent_id`
chain describes their state. A Zend frame is built lazily from it when an
observer needs one (backtraces, exception traces, warnings, observers). This
is exact reconstruction under the section 1 contract.

Inlining needs bail edges in the machine IR, where the allocator sees them,
and lazy frames for inlined callees, so that observing slow paths run in
place. Splicing callee bodies at the source-opline level, with a bail to the
original call before every operation that could observe the frame, is not
used: the bail tests and the saved run-time cache cost more than the call.

### 6. Unchanged rules

- No production VM fallback, no reuse of Zend VM opcode handlers, no Zend
  JIT IR and no second register allocator.
- PHP-observable behaviour is identical at every observation point:
  refcount, destructor and GC-root ordering, warnings and their order,
  exceptions, observers, backtraces, `func_get_args`, generators and fibers.
- Public Zend headers stay C-compatible. Extensions see a valid `EX` chain at
  every transition into C code.

## Consequences

- The Zend frame is no longer the only copy of a function's state. Every
  change to generated code, helpers or observation points must keep the
  frame-state maps complete.
- The acceptance measure is the total per warm request against stock PHP.
  The generic native code must be competitive on its own; speculation and
  inlining add to it.
- Existing mechanisms are extended rather than duplicated: frame states,
  entry cells, generations, the resume contract, and
  `generate_guarded_direct_exit()` for guard exits.
- Lazy slots (section 1) and helper primitives (section 2) go together,
  since a helper that reads frame slots forces their materialization. Native
  calls (section 3) are independent; speculation (section 4) needs the
  frame-state maps; inlining (section 5) needs sections 3 and 4.
- Compile time grows with specialization and inlining. The generic version
  stays single-pass TPDE; specialized versions are compiled only for hot
  functions.

## Alternatives

- **Every value in the Zend frame, plus inline forms per opcode:** cannot
  remove the cost of keeping two representations in sync.
- **Fully lazy frames without an eager header:** make every C transition
  depend on native stack walking. They come only with inlining (section 5).
- **Deoptimization into the VM:** keeps the VM in the production path, so it
  is excluded.
- **Guards only at function entry:** cannot specialize values loaded inside
  a function (array elements, properties, call results). Entry variants
  remain one form of specialization (ADR 0024).

## Verification

- Frame-state maps are verified before code publication.
- PHPTs cover each observation class of the context table in plain,
  specialized and inlined code, under low and high register pressure. The
  classes are warnings, exceptions and traces, backtraces with modified
  parameters, observers, dynamic-scope functions, generators and fibers.
  Tests combine them with aliasing, pending calls, side effects and
  suspension.
- Every guard and transfer class of deoptimization has a PHPT that forces it
  (one per class, not one per dynamic instance), with output compared
  against stock PHP. `scripts/native/test-phpt.sh --deopt-stress PERIOD`
  runs the debug suites under the stress mode, and the full tier includes
  period 1.
- The WordPress page check (`/`, `/?s=hello`, `/sample-page/`) and the full
  PHPT tier (debug, ASan, UBSan) run before every push. No sanitizer failure
  is accepted.
