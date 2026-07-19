# ADR 0017: Durable wave receipts

## Status

Accepted.

## Context

Task-result directories are external execution evidence. They are useful for a
single gate run, but an empty directory makes completed waves appear missing
and cannot establish durable provenance. Coverage reports describe what was
covered; they do not prove which commit and tree were tested.

## Decision

The committed wave ledger is the default status source. A committed receipt
binds a wave state to a subject commit and tree, the wave definition, profiles,
coverage report, command identities, and distinct immutable log digests.
Receipt states are `unsealed`, `revalidated`, `sealed`, and `invalid`.

Historical waves without receipts remain `unsealed`. Revalidation may produce
`revalidated`, but never retroactively produces `sealed`. A receipt names its
already existing subject commit, avoiding a self-hash cycle. `created_at` is
evidence metadata and is excluded from the deterministic dashboard.

The task-result schema is extended additively with a non-circular sealed-result
binding. Existing results continue to use `head_commit`. A committed sealed
result instead uses `tested_head_commit` for the already tested gate head,
keeps `head_commit` null, and binds the containing receipt by relative path and
SHA-256 in `seal_subject`. External result aggregation is available through the
explicit `check-results` and `render-results` commands; legacy invocations that
pass `--results-dir` remain accepted during the migration.

Prior-wave coverage reports are historical source snapshots, not mutable
inventories of every later wave. Their validators bind the recorded inventory
to an explicit committed subject and continue to run the owning wave's
functional checks against the live tree. This prevents additive changes to
shared MIR files from either rewriting historical evidence or making later
waves fail solely because the historical inventory changed.

## Consequences

Status, coverage, transient results, and durable provenance have separate
roles. Receipt verification rejects commit/tree drift, repository-artifact
hash drift, duplicate command/log identities, and optional external-log
corruption when an artifact root is provided.
