# ADR 0007: Relocatable OPcache persistence

## Status

Accepted.

## Context

OPcache images can outlive address-space layouts and process-local allocations.
Persisting raw code, heap, function, entry-cell, or metadata pointers would make
an image unsafe or meaningless when mapped by another process.

## Decision

Do not store process-local raw pointers in persistent OPcache images. A
script's native code is persisted as one pointer-free bundle per script
(`zend_native_compiler_serialize_bundle()`), stored with the script in shared
memory or the file cache. The bundle holds validated identifiers, offsets,
indices, relocations, and immutable provenance. Each process imports it
(`zend_native_compiler_import_bundle()`) and reconstructs process-local
pointers and entry cells after mapping, under bounds, alignment, type,
version, and code-identity validation.

Version persistent formats. The bundle header carries a magic, the bundle
format, the target, `ZEND_NATIVE_RUNTIME_ABI_VERSION`, the total size and a
checksum; the ABI version is also part of the OPcache system id and the image
build id. Reject or invalidate an incompatible, incomplete, or unverifiable
image; never guess compatibility or partially activate it.

## Consequences

- Persistent metadata must distinguish serialized identity from live addresses.
- Loading has an explicit validation and relocation phase before publication.
- Format evolution carries compatibility and invalidation rules: a change to
  emitted code or to a structure shared with generated code bumps
  `ZEND_NATIVE_RUNTIME_ABI_VERSION`.
- Process-local native state is rebuilt rather than serialized by address.
- Generated code of a persistable image embeds no process address: it
  reaches executor globals (`EG(exception)`, `EG(current_execute_data)`, the
  VM stack, the symbol table) and handler tables (`zend_flf_handlers`)
  through `zend_native_execution_context`, and call targets through relocated
  image symbols. Code that relies on per-process state chosen at compile time
  (observers, `zend_execute_internal`) is valid because that state is part
  of the system id. Only tier-2 code, which is never serialized, may embed
  process addresses.

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
