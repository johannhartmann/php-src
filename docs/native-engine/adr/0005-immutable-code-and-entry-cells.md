# ADR 0005: Immutable code versions and entry cells

## Status

Accepted on 2026-07-17.

## Context

In-place mutation of executable code or metadata races with active callers,
complicates rollback, and can pair machine code with the wrong relocations,
resume metadata, or persistent provenance.

## Decision

Treat a published native code version and all metadata it addresses as
immutable. Assign each version a stable identity and keep it alive while any
entry cell, active frame, resume target, or retained metadata can reference it.

Route calls through entry cells whose target can be published atomically after
the complete candidate version is built, relocated, validated, and made
executable. Replacement publishes a new version; it does not patch the old
version's semantic content in place. Reclamation waits for a proven quiescence
or equivalent lifetime mechanism.

## Consequences

- Readers see either the old complete version or the new complete version.
- Rollback is an entry-cell publication to a retained validated version.
- Code and metadata allocation, publication, and reclamation become explicit
  lifecycle stages.
- Non-semantic platform patching, if unavoidable, must complete before
  publication and may not race with execution.

## Alternatives

- In-place recompilation was rejected because readers could observe mixed code
  and metadata.
- Updating direct call sites individually was rejected because publication would
  not be atomic and rollback would require finding every caller.

## Verification impact

Concurrency tests must exercise publication and reclamation with active calls.
Validators must bind code, relocations, resume targets, and provenance to one
version identity. Tests must detect stale entry and use-after-reclamation paths.
