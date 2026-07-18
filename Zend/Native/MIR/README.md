# Canonical ZNMIR contract

This directory defines the single, target-neutral middle representation used
by the native engine. The W02 contract is an internal C ABI: it lets core,
analysis, text, frame-state, and verifier work proceed independently without
exposing storage layout.

## Contract layers

- `zend_mir.h` exposes opaque entity types, immutable record snapshots, an
  allocator vtable, a read-only view, and the controlled mutator/builder API.
  It also freezes storage-independent text dump/parser and stage-one verifier
  entry points implemented by their owning W02 tracks.
- `zend_mir_ids.h` owns contract version 1.0 and all 32-bit identity rules.
- `zend_mir_opcodes.h` freezes the minimal core opcode, representation, and
  target-neutral constant-kind catalogs. PHP-specific opcodes may only be
  added later.
- `zend_mir_effects.h` binds the W01 effect, domain, ownership, action, and
  barrier catalogs, plus predicates, guard facts, and composition rules, to
  exact numeric values.
- `zend_mir_frame_state.h` binds the W01 frame, safepoint, continuation, and
  resume enums and exposes immutable, ID-only references.
- `zend_mir_diagnostic.h` defines stable codes, locations, fixed-size messages,
  and a bounded process-local sink.
- `zend_mir_private.h` is the only storage-binding layer. Its pointers are
  process-local and are never persistent MIR identity.

The fixture host in `tests/native/mir/contracts` implements the view and
mutator over fixed arrays solely for unit tests. It is not linked into PHP or a
production library and intentionally has no opcode dispatcher or execution
semantics.

## Identity and failure rules

Every identity is a 32-bit integer. `0xffffffff` is the sole invalid ID and
`0xfffffffe` is the largest general valid ID. Value IDs below `0x80000000` are
original Zend SSA values; values from `0x80000000` through `0xfffffffe` are
synthetic. Construction that would overflow either namespace returns the
invalid sentinel.

Catalog enums use contiguous non-negative values and `-1` as their invalid
sentinel. This keeps their declarations strictly valid in C11; enum sentinels
are not persistent MIR identities and are distinct from `ZEND_MIR_ID_INVALID`.

View callbacks return `false` for an invalid index or ID. Mutator callbacks
return `false` before committing a partial change when an ID, capacity,
relationship, or allocation cannot be represented. Implementations should
emit a stable diagnostic when a sink remains below its configured limit.
Unknown opcodes, effects, ownership, frame data, or contract versions fail
closed and prevent publication or lowering.

Constants live in an immutable pool keyed by their result value ID. Integer
and double payloads are canonical bits; null and booleans have no payload;
strings and semantic pointers use symbol IDs. No constant contains a host
pointer or target encoding. A PHI operand at index N belongs to predecessor N.
Conditional branches use successor 0 for true and successor 1 for false.

## Versioning

`ZEND_MIR_CONTRACT_VERSION` encodes a 16-bit major and 16-bit minor version.
Version 1.1 adds a stable source-map table associating a source-position ID,
op-array ID, opline index and phase with its owning frame-state ID. It carries
no generated-code location and does not change the canonical 1.0 text grammar.
Consumers accept the same major and a minor no newer than they implement.
Within major version 1, extensions are additive only: existing numbers,
meanings, callback signatures, and record fields cannot be removed, reordered,
or reused. Adding an enum value, callback, or record field increments the minor
version and requires deterministic text, verifier, C11, and C++20 coverage.
Changing an existing meaning or representation requires a new major version
and a compatibility ADR.

No public contract contains target registers, instruction encodings, machine
offsets, TPDE types, persistent raw pointers, VM dispatch, or a VM fallback.
