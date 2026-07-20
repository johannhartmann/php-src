# W05-v2 single-writer contract

W05-v2 has one linear writer on `integration/wave-06`, pinned to contract
commit `e164f851875a621858058fa5d641cdf1477c1466`. There are no parallel writing
tracks and no merge commits.

`W05-integration-gate` is the only wave task. Its owned paths are generated
from the implementation, gate, and seal phases in
`docs/native-engine/calls/w05-phase-manifest.json`. QH contract paths and this
QP definition are immutable after the pin.

The task result carries QH, QP, QM, and QG phase receipts. Each receipt binds
the commit, tree, parent, exact changed paths, trailer, and command-summary
digests. The final W05 receipt subjects QG; QS is the containing seal commit and
is intentionally not embedded in its own content.

After a clean QM, exactly two external read-only reviews inspect that same QM:
one semantics review and one evidence/history/capability review. QG commits
their JSON payloads unchanged. A production finding is repaired only in a
repeatable `W05-v2-implementation` commit and both reviews are repeated.

W05 remains a call model. It executes no MIR or VM handler, introduces no
fallback or C ABI, emits no target code, and remains
`codegen_eligible=false`.
