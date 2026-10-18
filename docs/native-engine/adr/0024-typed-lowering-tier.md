# ADR 0024: Typed lowering

## Status

Accepted. Linux x86-64 only: the compiler lowers through
`zend_mir_lower_typed_zend_op_array()` for the Linux x64 target and through
`zend_mir_lower_zend_op_array()` for Darwin arm64
(`Zend/Native/Compiler/zend_native_compiler.c`).

## Context

OPcache's type inference often proves the type, range and refcount state of
a value. Keeping such a value boxed in a zval costs type tests, boxed loads
and stores and helper calls that the proof makes unnecessary. ADR 0025 makes
native state canonical, so a proven value can live in a register in its
machine representation.

## Decision

Lowering types every operation it can from Zend's type inference. Every
operation it does not type keeps the boxed executable value operation.

1. **Value classes.** Each SSA value gets an exact scalar type and facts from
   `var_info` (integer range, finite, non-refcounted). The machine plan
   (`freeze_component_machine_plan()`,
   `Zend/Native/TPDE/Common/zend_tpde_machine_plan.cpp`) gives it a machine
   representation: `I64`, `F64`, `BOOL`, a string, array, object, reference
   or resource pointer with an ownership transfer, or a boxed zval
   (`zend_tpde_machine_value_kind`). Proven facts get no guard.
2. **Values that stay in their frame slot.** At least these values keep their
   frame slot authoritative:
   - values bound to references, aliases, globals or statics;
   - CVs that dynamic variable access, `include` or `eval` can reach;
   - CVs live into `catch` or `finally`;
   - values live across a generator suspension. Generator functions are not
     lowered typed at all.
3. **Entry specialization.** Unknown argument types may be checked once at
   function entry before a specialized body runs. Examples are the integer
   (or `int|float`) variant of a function with untyped integer parameters
   and the argument guards of a tier-2 copy (ADR 0025 section 4). Values
   learned inside a function are specialized through speculation and
   deoptimization (ADR 0025 section 4).
4. **Per-opline typing.** A rule selected by the operand facts lowers each
   opline. When no typed rule applies, only that opline stays a boxed value
   operation, with explicit box and unbox at its boundary. A missing typed
   rule never rejects a whole function.
5. **MIR.** MIR carries the typed scalar operations (`zend_mir_opcodes.h`):
   - `i64_*_no_overflow` when the operand ranges exclude overflow;
   - `f64_add`, `f64_sub` and `f64_mul`, where a long operand mixed with a
     double is converted as the VM does;
   - integer and double comparisons and conversions.

   Other operations, such as division, are executable value operations
   (`value_binary_op`) whose operands carry their exact types. The MIR
   verifier covers all of them over the whole CFG.
6. **Backend.**
   - Box and unbox are adaptor nodes (`BoxScalar`, `UnboxScalar`,
     `UnboxPointer`).
   - Operations that cannot fail emit no guard. A guarded operation is a
     `GuardedFast`/`GuardedCold` pair, and its slow path is an out-of-line
     block in the cold area.
   - A slow edge that completes an operation normally may rejoin the hot
     path at a merge that keeps the typed representations of live values.
   - A value operation on proven numbers computes inline: a long result
     that overflows continues in double, and division computes in double.
   - Only an observation point (ADR 0025 section 1) forces values into Zend
     slots.
   - Typed helpers take native arguments (`zend_long`, `double`,
     `zend_string *`, `HashTable *`), not encoded operands.

## Consequences

- Typed code and boxed code coexist in one function. Every boundary between
  them is an explicit box or unbox.
- Every change is checked against stock PHP with the native PHPTs, the debug,
  ASan and UBSan suites and the WordPress page checks.
- Local changes to the vendored TPDE are recorded in
  `Zend/Native/TPDE/ThirdParty/tpde/PATCHES.md`.
