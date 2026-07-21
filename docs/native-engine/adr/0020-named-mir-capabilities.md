# ADR 0020: Named MIR capabilities instead of stage numbers

## Status

Accepted.

## Decision

W05 does not add `STAGE4_VERIFIED`. Linear stage numbers stop being useful once
features mature at different rates, so the W05 lowering result reports named
capabilities and named semantic debts.

The call model reports scalar semantics, reducible control flow, direct user
call modeling, caller and callee frame models, and abstract call effects. It
also keeps runtime binding, exception and bailout cleanup, refcounted transfer,
protected continuations, dynamic targets, observers, COW/indirect semantics,
and internal C-ABI interoperability explicitly unsupported.

`zend_mir_verify_w05_calls()` validates the source-backed call tables directly.
The final structural, scalar, control-flow, and call-model checks all run before
the module is returned. Passing results are not copied into persistent MIR
records. A successful result is `modeled = true` and
`codegen_eligible = false`.

W04 Stage 1, 2, and 3 are verified before W05 mutation and retained in the W05
result as `prerequisite_guarantees`. The final module reports only `FINALIZED`;
the final W05 verifier checks the complete extension-aware module.

## Consequences

W03 and W04 guarantees retain their bit identities. A failed W05 result
contains no module, prerequisite guarantees, capabilities, debts, or partial
plan. Consumers inspect the successful result's capability and debt fields;
there is no receipt, ledger, or evidence-publication protocol.
