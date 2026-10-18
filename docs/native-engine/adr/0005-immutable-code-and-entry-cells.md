# ADR 0005: Immutable code versions and entry cells

## Status

Accepted.

## Context

In-place mutation of executable code or metadata races with active callers,
complicates replacement, and can pair machine code with the wrong relocations,
resume metadata, or persistent provenance.

## Decision

Treat a published native code version and all metadata it addresses as
immutable. Assign each version a stable identity and keep it alive while any
entry cell, active frame, resume target, or retained metadata can reference it.

Route calls through entry cells (`zend_native_entry_cell`,
`Zend/Native/Runtime/Common/zend_native_calls.h`) whose code word is published
atomically after the complete candidate version is built, relocated,
validated, and made executable. Publication stores the code with release
ordering; callers load it with acquire ordering, and a loaded code pointer pins
that version for the activation. Replacement, such as a tier-2 recompilation,
publishes a new version in the cell; frames still running the old version
finish there. It does not patch the old version's semantic content in place.
Reclamation waits until the cell's active calls and suspended frames have
drained (`zend_native_compiler_is_quiescent()`).

## Consequences

- Readers see either the old complete version or the new complete version.
- Code and metadata allocation, publication, and reclamation are explicit
  lifecycle stages.
- Non-semantic platform patching, if unavoidable, must complete before
  publication and may not race with execution.

## Alternatives

- In-place recompilation was rejected because readers could observe mixed code
  and metadata.
- Updating direct call sites individually was rejected because publication would
  not be atomic and replacement would require finding every caller.

## Verification impact

Concurrency tests must exercise publication and reclamation with active calls.
Validators must bind code, relocations, resume targets, and provenance to one
version identity. Tests must detect stale entry and use-after-reclamation paths.
