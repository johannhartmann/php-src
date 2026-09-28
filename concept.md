# Native x64 performance concept after the typed tier

Status at `60439c44cc3` (2026-09-28). This combines two external reviews, the
profiles taken while implementing ADR 0024 phase 1, and the source locations
checked against that commit. It sets the order of the next work and two
amendments to ADR 0024.

## 1. Where we stand

Kernel harness: `scripts/native/benchmark-bench-kernels.py`, x64 product
release with OPcache, against stock PHP `47355da`. Each kernel runs in its own
process, is warmed up once, and the best of several runs counts.

| Kernel | Native | Stock | Ratio | Gap |
|---|---:|---:|---:|---:|
| simple, simplecall, simpleucall, simpleudcall | 0.8–1.5 ms | 2.3–4.7 ms | 0.32 | — |
| mandel | 11.7 ms | 21.5 ms | 0.55 | — |
| mandel2 | 13.6 ms | 32.2 ms | 0.42 | — |
| ary3 | 16.9 ms | 24.1 ms | 0.70 | — |
| sieve | 4.8 ms | 6.5 ms | 0.73 | — |
| matrix | 14.5 ms | 13.2 ms | 1.10 | 1.3 ms |
| fibo | 52.3 ms | 42.6 ms | 1.23 | 9.7 ms |
| ackermann | 16.4 ms | 11.0 ms | 1.48 | 5.4 ms |
| ary, ary2 | 4.4 ms | 2.8 ms | 1.55 | 1.5 ms each |
| nestedloop | 23.3 ms | 13.9 ms | 1.68 | 9.5 ms |
| heapsort | 24.0 ms | 12.8 ms | 1.87 | 11.2 ms |
| hash1 | 12.9 ms | 5.5 ms | 2.35 | 7.4 ms |
| strcat | 7.7 ms | 3.0 ms | 2.60 | 4.7 ms |
| hash2 | 20.1 ms | 5.1 ms | 3.95 | 15.0 ms |
| **Geomean** | | | **0.96** | |

`Zend/bench.php` in total: 0.238 s native against 0.211 s stock (0.386 s
before the typed tier).

Where a computation reaches TPDE as a continuous chain of typed scalar
operations, native code is already faster than stock PHP (mandel, simple
loops, sieve). The remaining gaps lie at the boundaries to dynamic types,
heap values and calls. TPDE's code quality is not the limit anywhere; the
limit is what the integration hands it.

## 2. Diagnosis

The integration still too often optimizes single operations instead of the
path of a value through a computation. Three patterns cause almost all of the
remaining gap:

1. **Producer and consumer do not match.** A faster operation produces a
   representation its consumer cannot take, so the value is published to its
   Zend slot and read back. Commit `ac24a60d915` read array doubles into
   registers; the following comparison accepted only frame operands or
   integers, and heapsort went from 24 to 27 ms (reverted in `60439c44cc3`).
2. **Generic runtime protocols for cases the compiler has already decided.**
   Helpers decode an explicit operation, choose the operator at runtime
   (`get_binary_op`), resolve operands and references, and run the general
   assignment and cleanup protocol, even when the operator, operand forms and
   target are static. Examples: `ASSIGN_DIM_OP`, `ASSIGN_OP`, the array
   iterator, the frameless call wrapper.
3. **Types known only at runtime are classified again on every iteration or
   call.** Untyped parameters (`$n` in nestedloop, fibo, ackermann) keep whole
   loops and recursive calls on boxed paths.

Protocol cost is not the same as semantic cost. The general helpers stay as
the owners of rare cases (references, missing keys, overflow, copy-on-write,
destructors, errors). The work is to stop sending already narrowed common
cases through them.

## 3. Amendments to ADR 0024

Two rules in ADR 0024 are too absolute for the next steps. Proposed wording:

1. **Guards.** Replace "No speculation, no guards, no deoptimization" with:
   *No redundant guards for proven facts. A bounded entry specialization may
   check the types of unknown inputs once, before a specialized body or loop
   runs; other inputs take the general native form. There is no
   deoptimization and no VM fallback.*
2. **Slow edges.** Replace "Slow edges are out-of-line blocks that do not
   merge back into the hot path" with: *Slow edges are out-of-line blocks. A
   slow edge that completes an operation normally may rejoin the hot path at a
   merge that keeps the typed representations of live values; only an
   observation boundary forces materialization into Zend slots.* The current
   machine CFG already routes cold blocks back to the continuation.

Both change an accepted decision and need approval before they are applied to
`docs/native-engine/adr/0024-typed-lowering-tier.md`.

## 4. Work packages

Ordered by dependency and effort; the gap column is the upper bound of what a
package can win on `Zend/bench.php`.

| # | Package | Kernels | Gap |
|---|---|---|---:|
| 1 | Close the double consumer, then re-enable the double array read | heapsort, matrix | ~12 ms |
| 2 | Specialize array foreach and compound updates of existing buckets | hash2, ary, ary2 | ~18 ms |
| 3 | Remove generic call and operator wrappers (frameless, string append) | hash1, strcat | ~12 ms |
| 4 | Entry specialization and typed calls | nestedloop, fibo, ackermann | ~25 ms |

### Package 1: double consumer

Goal: array read → numeric comparison → branch stays in registers.

- Scalar comparisons require both F64 operands to be `FINITE`
  (`Zend/Native/Lowering/Core/zend_mir_lowering_providers.c:1434`, `:1595`,
  `:1855`). Lower double comparisons without that fact, with IEEE semantics:
  unordered operands make `<`, `<=`, `==` false and `!=` true. The frame form
  in the x64 emitter already does this (`UCOMISD` plus the parity flag).
  Check which MIR opcodes and verifier rules depend on `FINITE` before
  relaxing it.
- `numeric_framed_binary` in `Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp`
  accepts register operands only as boxed zvals or exact I64 values. Accept
  F64 machine values and boxed zvals holding doubles, and use known operand
  types for register operands as for frame operands (the `!register_operands`
  condition at about line 8499).
- Then re-apply the read form reverted in `60439c44cc3`: register-key packed
  reads return any non-refcounted scalar in a boxed register, and dereference
  a CV that holds the array by reference.

Success: heapsort gets faster than 24 ms as a whole; a regression test covers
NaN, INF and mixed int/double comparisons read from arrays.

**Result (2026-09-28).** Typed double comparisons no longer need FINITE facts:
the x64 emitter compares with IEEE semantics and the logic provider accepts
non-finite operands under a new `IEEE_F64_COMPARE` proof, set only by the typed
tier (DarwinA64 keeps the requirement). This made more functions typed bodies
and exposed two latent typed-body faults, now fixed: TPDE handed out XMM0/XMM1
as fixed registers, which blocked double returns after a branch, and a
returned pi had its COPY elided. Heapsort did not change: its keys are
`long|double` and its elements come from a by-reference untyped array, so the
comparisons stay boxed. Re-applying the wider register read still costs about
12 % more instructions in heapsort by itself (measured with the harness
`--perf`), so the read form stays reverted; the cost lies in the read path,
not in its consumer. Heapsort moves to package 4 (typed parameters).

### Package 2: foreach and compound array updates

`foreach ($hash1 as $key => $value) { $hash2[$key] += $value; }` spends its
time in `zend_native_value_iterator_branch`, `zend_native_iterator_set_key`,
`zend_native_iterator_assign_value` and `zend_native_value_assign_dim_impl`
(`compound = true`).

- **Iterator:** once `FE_RESET_R` has chosen a by-value array iteration, the
  per-element step (`FE_FETCH_R`) works directly on the array, its position and
  the next bucket: skip holes, publish the key (integer or interned string with
  addref) and assign the value to a CV that holds no reference and no
  refcounted value. Objects, by-reference iteration, a changed array and
  refcounted old values keep the general step.
- **`ASSIGN_DIM_OP`:** the operator is known at compile time. For an existing,
  unreferenced bucket holding an integer or double, with a numeric right
  operand and `+`, `-` or `*`, compute in place (integer overflow to double as
  `fast_long_add_function` does). Missing keys (warning), references,
  copy-on-write separation, strings and other operators keep
  `zend_native_value_assign_dim_impl`.
- The specialized paths get their own precise effect description; do not mark
  the general helpers' effects as harmless.

Success: hash2 well below 3.95; ary and ary2 improve through the same
assignment path.

**Result (2026-09-28).** `zend_native_value_assign_dim_op` first tries an
in-place update (existing `long`/`double` element, numeric operand, `+ - *`,
unused result, unshared array) in a separate function, so the plain write
path of `zend_native_value_assign_dim_impl` keeps its code (inlining the check
there slowed ary by 0.4 ms). The foreach value assignment copies into a CV
that holds no counted value directly, as the VM does. hash2 3.56 -> 2.55; the
per-element iterator protocol (explicit operation decoding, key publication)
remains and is the next step if hash2 stays a priority.

### Package 3: call and operator wrappers

- **Frameless calls** (`zend_native_call_frameless_internal`,
  `Zend/Native/Runtime/Common/zend_native_calls.c`, about line 973): opcode,
  handler index, argument count and operand forms are static. Emit a direct
  call of the `zend_frameless_function_N` handler with the argument addresses
  and the result slot, bound through the existing image symbol/relocation
  mechanism, and consume temporary arguments inline. Observed calls, dynamic
  resolution and required argument conversions keep the wrapper.
- **String append** (`ASSIGN_OP` with `ZEND_CONCAT`): skip the explicit
  operation decoding and `get_binary_op`. For a CV target that holds a
  non-reference string and a string or literal operand, call Zend's
  concatenation with the known addresses (`concat_function(var, var, value)`
  keeps in-place extension). No rope implementation, no allocator of our own.

Success: hash1 and strcat lose the wrapper and decoding share of their
profiles (`zend_native_call_frameless_internal`,
`zend_native_frameless_decode_operand`, `zend_native_value_assign_op`,
`zend_native_value_init_explicit_operation`); `concat_function` and
`_erealloc` stay.

**Result (2026-09-28).** Two helpers take operand offsets precomputed by the
compiler instead of encoded operands: `zend_native_call_frameless_direct()`
(literal, CV and temporary arguments; observers, undefined CVs and references
as before) and `zend_native_value_concat_assign_direct()`, which builds the
operation record from offsets and calls `concat_function` without
`get_binary_op`. A call with the handler bound directly in native code needs
image relocations for Zend's handler table and stays open. hash1 2.39 ->
2.05, strcat 2.60 -> 2.05.

### Package 4: entry specialization and typed calls

Needs the ADR amendment in section 3. Three separate problems:

1. **Unknown parameter types.** For a function whose body is typed under
   integer (or double) parameters, compile a specialized body with those
   parameter facts (the compiler already accepts supplied argument types) and
   check the argument types once at entry. Callers with statically known
   argument types call the specialized body directly; the general entry keeps
   the current native form. The public PHP signature does not change.
2. **Loop invariants and ranges.** Inside a specialized loop, an unchanged
   bound such as `$n` is classified once; induction variables keep their
   proven ranges through PHIs. A counter that may still overflow (`$x++` in
   nestedloop) gets an overflow edge to double
   (`long|double` MIR operations, ADR 0024 item 3) instead of the general
   increment.
3. **Recursive and nested calls.** `freeze_component_machine_plan` excludes
   typed bodies for any function with nested direct calls
   (`Zend/Native/TPDE/Common/zend_tpde_backend.cpp:12044`), and
   `freeze_typed_body_signature` rejects effectful body operations and phi
   inputs without a typed definition. Implement result binding for nested
   typed calls instead of inverting the exclusion. Known integer arguments do
   not prove an integer result: recursive results (`fibo_r(...) + fibo_r(...)`)
   need the overflow edge and exception propagation of the typed-call ABI.

Success: nestedloop runs without per-iteration type classification; fibo and
ackermann take typed component calls on their recursive edges.

## 5. Rules for every package

- **Optimize chains, not operations.** A new producer form lands only
  together with its consumers; measure the kernel as a whole.
- **Measure before and after** with the kernel harness on a product release
  build, and profile with `perf` (`nix shell nixpkgs#perf`). Scripts written
  just before a run need `-d opcache.file_update_protection=0`, otherwise
  OPcache does not optimize them and profiles show unoptimized call paths.
  Build with the scratch `NATIVE_WORK_ROOT`, otherwise the benchmarked binary
  is stale. Repeat noisy kernels (heapsort varies by about 1 ms).
- **Keep semantics in the general paths.** Every specialized path has a
  complete fallback to the existing helper before it mutates state.
- **Test tiers:** `scripts/native/test-phpt.sh --tier quick` per change,
  `--tier commit` before a commit, `--tier full` before a push. Each package
  adds PHPTs whose expectations come from stock PHP.

## 6. Not planned

- New runtime validation, removal of the general helpers, duplicated slow
  paths, or another architecture change.
- Benchmark recognition: every specialization is keyed on operand forms and
  facts, not on kernels.
- Compile-time work (repeated call-site scans, `codegen_ns` including planning)
  stays secondary while warm execution is the target.
