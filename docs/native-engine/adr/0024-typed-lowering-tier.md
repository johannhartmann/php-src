# ADR 0024: A typed lowering tier replaces the W11 scalar overlay

## Status

Accepted. Linux x64 only; DarwinA64 keeps the W11 path. The rule "there is no deoptimization" is superseded by [ADR 0025](0025-native-canonical-execution.md), which adds speculation with deoptimization into generic native code.

## Context

Native code is slower than the stock VM with OPcache (Zend/bench.php
0.386 s against 0.212 s at 9452a49b311). Almost every PHP value is a boxed
zval in its canonical frame slot, so each operation loads a zval, tests its
type, computes, and stores a zval, with a guarded fast path and a cold
helper. The VM does the same work with handlers specialized by the same
OPcache type inference, so native code has no structural advantage.

The W11 overlay cannot close the gap by adding opcodes:

- It scalarizes only ASSIGN, QM_ASSIGN, guarded long increments,
  comparisons and RETURN, and erases the inferred fact of every other value
  operation. Arithmetic, strings, arrays and loop accumulators stay boxed.
- One PHI input without a fact makes the whole connected PHI component a
  zval.
- Its scalar providers reject duplicate operand uses, mixed long/double
  operands, division and integer operations without range proofs, and any
  rejection discards the whole function (MIRL0007).
- Strings, arrays and objects have no MIR representation other than ZVAL.

OPcache's type inference is sound: an exact `[long]` result already
excludes overflow, and an exact type needs no runtime guard.

## Decision

A new typed lowering tier replaces the W11 fact model, scalarization
decisions and boxed-materialization planning. It keeps the W04 control-flow
lowering, the W05 call model and the W09 executable value operations, which
remain the boxed form of any operation the tier does not type.

1. **Type plan.** Before lowering, each SSA variable receives a class from
   `var_info`: `I64` (with range), `F64`, `BOOL`, `NULL`, `STRING`, `ARRAY`,
   `OBJECT` or `BOXED`, plus its refcount information and an escape class.
   Values that must stay authoritative in their canonical frame slot are
   those bound to references, aliases, globals or statics, CVs visible to
   dynamic variable access or include, CVs live into catch or finally, and
   values live across a generator suspension. No redundant guards for
   proven facts. A bounded entry specialization may check the types of
   unknown inputs once, before a specialized body or loop runs; other
   inputs take the general native form. There is no deoptimization and no
   VM fallback.
2. **Per-opline typing.** Each opline is lowered by a rule selected by its
   operand classes. When no rule applies, only that opline becomes a boxed
   W09 operation, with explicit box and unbox at its boundary. The tier
   never rejects a whole function because a rule is missing, and there is
   no VM fallback.
3. **MIR additions**, appended after the existing catalogs (ADR 0011):
   typed double division, integer operations with an overflow edge to
   double for `long|double` results, double operations without a FINITE
   requirement, implemented BOX/UNBOX primitives, typed string and array
   pointer representations with ownership, and typed array and string
   operations with a slow edge. A verifier covers them over the whole CFG.
4. **Backend.** Operations that cannot fail emit no guarded diamond. Slow
   edges are out-of-line blocks. A slow edge that completes an operation
   normally may rejoin the hot path at a merge that keeps the typed
   representations of live values; only an observation boundary forces
   materialization into Zend slots.
   Typed helpers take native arguments (`zend_long`, `double`,
   `zend_string *`, `HashTable *`) instead of encoded operands.
5. **Target gating.** The tier runs only for Linux x64 (`linux_inline_forms`).
   DarwinA64 rejects the new opcodes explicitly and keeps the W11 path.

## Delivery

Phases, each gated by differential tests against stock PHP, the debug,
ASan and UBSan suites, the contract validators and a harness run without
regressions:

0. Per-benchmark kernels, instruction and branch counts, differential
   harness.
1. Type plan and scalar tier: long, double, bool, comparisons, branches,
   increments, assignments, PHIs and loops.
2. Backend: slow edges without merges; register pressure at loop headers.
3. Packed arrays with integer keys, foreach.
4. Strings and hash arrays; typed helper ABI.
5. Typed calls: the typed-body ABI extended to loops, with an on-demand
   frame for helpers, exceptions and backtraces.
6. Removal of the W11 scalar overlay and of per-operation inline forms the
   tier covers.

## Amendments

2026-09-28 (`concept.md`): entry specialization for unknown input types and
slow edges that rejoin at typed merges, replacing "no guards" and "slow edges
do not merge back". The work order after phase 1 follows `concept.md`
section 4 rather than the phase list.

## Consequences

Lowering profiles, `check-contract.py` bounds (division is currently pinned
as deferred), the semantic catalog, MIR goldens and text labels change as
the MIR additions land. Changes to the vendored TPDE need a separate
decision.
