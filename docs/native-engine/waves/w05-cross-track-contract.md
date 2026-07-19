# W05 single-writer contract

W05 has one linear writer history on `integration/wave-05`. The contract base
is `63a5070daa91da9e702d0ff52ea4d77c20ad89e6`; every W05 task result declares
that commit as both expected and actual base. The wave-pin commit P, the
implementation M or repaired M', the gate G', and the durable seal S remain in
one ancestry chain.

## No writing tracks

`parallel_tracks` is empty. `W05-integration-gate` is the only wave task and
owns the serial implementation, gate, and seal paths frozen by
`docs/native-engine/calls/w05-phase-manifest.json`.

W05-R1 and W05-R2 are independent read-only reviews against the same exact
implementation head. Their JSON and Markdown artifacts remain external and
are bound by SHA-256 in the gate review manifest. They are evidence, not wave
tasks or writing branches.

## Immutable contracts and phase boundaries

All H contracts, profiles, root headers, diagnostics, schemas, and contract
tests are immutable after H. A contract error starts a new H/P cycle. The
implementation phase changes only its manifest paths. The gate may produce
CI, validation, report, and integration-golden artifacts but must not repair
production code. The seal may write only the receipt, task result, ledger, and
status and repairs nothing.

The only serial paths shared by program setup and seal are
`docs/native-engine/waves/ledger.json` and
`docs/native-engine/waves/status.md`.

## Result provenance

The single committed task result uses H as `expected_base_commit` and
`actual_base_commit`. Its tested head is G'; the final containing head is S and
is derived by receipt verification rather than embedded circularly. A result
from a different base, an unreviewed implementation head, or a non-linear
writer history is invalid.
