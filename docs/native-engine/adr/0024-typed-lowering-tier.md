# ADR 0024: Typed lowering

## Status

Accepted. Linux x64.

## Decision

Lowering types every operation it can from Zend's type inference and keeps the
boxed executable value operations as the form of every operation it does not
type.

1. **Type plan.** Before lowering, each SSA variable receives a class from
   `var_info`: `I64` (with range), `F64`, `BOOL`, `NULL`, `STRING`, `ARRAY`,
   `OBJECT` or `BOXED`, plus its refcount information and an escape class.
   Values bound to references, aliases, globals or statics, CVs visible to
   dynamic variable access or include, CVs live into catch or finally, and
   values live across a generator suspension stay authoritative in their
   frame slot. Proven facts get no guard. Unknown input types may be checked
   once by an entry specialization before a specialized body runs; values
   learned inside a function are specialized through speculation and
   deoptimization (ADR 0025).
2. **Per-opline typing.** Each opline is lowered by a rule selected by its
   operand classes. When no rule applies, only that opline becomes a boxed
   value operation, with explicit box and unbox at its boundary. A missing
   rule never rejects a whole function.
3. **MIR.** Typed double division, integer operations with an overflow edge
   to double for `long|double` results, double operations, BOX/UNBOX, typed
   string and array pointer representations with ownership, and typed array
   and string operations with a slow edge. The verifier covers them over the
   whole CFG.
4. **Backend.** Operations that cannot fail emit no guard. Slow edges are
   out-of-line blocks; a slow edge that completes an operation normally may
   rejoin the hot path at a merge that keeps the typed representations of
   live values. Only an observation point forces values into Zend slots.
   Typed helpers take native arguments (`zend_long`, `double`,
   `zend_string *`, `HashTable *`), not encoded operands.

## Consequences

Every change is checked against stock PHP with the native PHPTs, the debug,
ASan and UBSan suites and the WordPress page checks. Local changes to the
vendored TPDE are recorded in `Zend/Native/TPDE/ThirdParty/tpde/PATCHES.md`.
