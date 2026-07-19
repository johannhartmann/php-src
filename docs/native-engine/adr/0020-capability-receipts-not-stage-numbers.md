# ADR 0020: Capability receipts instead of stage numbers

## Status

Accepted.

## Decision

W05 does not add `STAGE4_VERIFIED`. Linear stage numbers stop being useful once
features mature at different rates, so W05 records named capabilities and
named semantic debts.

A successful W05 call-model receipt contains exactly these capabilities:

- scalar semantics;
- reducible control flow;
- direct user call model;
- caller frame model;
- callee entry model.

It also keeps these debts visible:

- call runtime binding;
- call exception propagation;
- call bailout/reentry;
- call observer integration;
- call result ownership;
- internal C-ABI interoperability.

`zend_mir_verify_w05_calls()` validates the source-backed call tables and the
receipt. A module containing such a call is `modeled = true` and
`codegen_eligible = false`. Later waves may discharge individual debts only by
publishing their own named evidence; no stage-number alias may hide an open
debt.

W04 Stage 1, 2, and 3 are verified before W05 mutation and are retained in the
W05 result as `prerequisite_guarantees`. They are not copied into the final
module's `lowering.guarantees`: the final module reports only `FINALIZED`.
`CALL_DIRECT_USER` deliberately remains outside the frozen W03 generic opcode
boundary and is validated on the final module by
`zend_mir_verify_w05_calls()`. The named scalar and reducible-control-flow
capabilities mean that W05 preserved the verified W04 projection while adding
only the source-backed call extension; they are not aliases for a rerun Stage
1, 2, or 3 claim.

## Consequences

W03 and W04 guarantees remain prerequisites and retain their frozen bit
identities. A failed W05 result contains no module, prerequisite guarantees,
capabilities, debts, or partial plan. Consumers must test the named receipt
rather than infer execution readiness from the newest numeric stage.
