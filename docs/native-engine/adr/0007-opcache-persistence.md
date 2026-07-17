# ADR 0007: Relocatable OPcache persistence

## Status

Accepted on 2026-07-17.

## Context

OPcache images can outlive address-space layouts and process-local allocations.
Persisting raw code, heap, function, entry-cell, or metadata pointers would make
an image unsafe or meaningless when mapped by another process.

## Decision

Do not store process-local raw pointers in persistent OPcache images. Store
validated identifiers, offsets, indices, relocations, and immutable provenance
instead. Reconstruct process-local pointers and entry cells after mapping, under
bounds, alignment, type, version, and code-identity validation.

Version persistent formats. Reject or invalidate an incompatible, incomplete,
or unverifiable image; never guess compatibility or partially activate it.

## Consequences

- Persistent metadata must distinguish serialized identity from live addresses.
- Loading has an explicit validation and relocation phase before publication.
- Format evolution carries compatibility and invalidation rules.
- Process-local native state is rebuilt rather than serialized by address.

## Alternatives

- Raw pointers with an assumed fixed mapping were rejected because ASLR,
  allocator state, and multi-process use invalidate the assumption.
- Best-effort loading was rejected because partial activation could publish
  mismatched code and metadata.

## Verification impact

Fixtures must load images at different mappings, corrupt each reference class,
exercise version mismatch and truncation, and prove rejection before
publication. Reviews must trace every persistent field to a validated
relocatable representation.
