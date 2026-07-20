# W06 single-writer contract

W06 has one linear writer on `integration/wave-06`, pinned to contract commit
`6483d2593c5ec877f63bd1625a1609ab762abb27`. There are no parallel writing
tracks, specialist branches, cherry-picks, or merge commits.

`W06-integration-gate` is the only wave task. Its owned paths are generated in
order from the A1 core, A2 lowering, gate, and seal phases in
`docs/native-engine/values/w06-phase-manifest.json`. H contract paths and this
P definition are immutable after the pin. Read-only R1 and R2 reviews own no
repository path.

The serial history is:

```text
W05-v2 seal -> H -> P -> A1 -> R1 -> A2 -> R2 -> repair -> gate -> seal
```

Confirmed A1 findings are repaired only in A1-owned paths; confirmed A2
findings only in A2-owned paths. Contract findings require a new H and P.
Gate code, corpus, or contract findings return to the owning earlier phase.

W06 models storage, references, indirect slots, ownership transfers, aliasing,
and separation requirements. It executes no MIR or VM handler, clones no
container, emits no target code, introduces no fallback, and remains
`codegen_eligible=false`.
