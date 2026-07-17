# ADR 0011: Canonical ZNMIR core contract

## Status

Accepted on 2026-07-17.

## Context

W01 freezes semantic catalogs but does not define the C boundary used by the
six W02 implementation tracks. Those tracks need stable identities, records,
storage-independent traversal and construction, and a way to execute tests
before the production arena exists.

## Decision

ZNMIR has one target-neutral internal C contract at version 1.0. Module,
function, block, instruction, value, and frame-state storage stays opaque.
Consumers use immutable record snapshots through `zend_mir_view`; construction
uses `zend_mir_mutator`. Allocation is supplied by a context-bearing vtable and
does not prescribe Zend, arena, or test allocation.

Text dumping writes through a process-local byte callback; parsing consumes a
bounded byte span through the mutator. Stage-one verification consumes only the
view. These signatures are frozen in the core header so the text and verifier
tracks can build executable tests without the production arena.

All identities are 32-bit. `0xffffffff` is the only invalid ID. Value IDs use
the high bit as a stable namespace tag: low-half IDs preserve original Zend SSA
identity and high-half IDs are synthetic. Synthetic payload `0x7fffffff` is
reserved because it would produce the invalid sentinel. Overflow fails before
mutation and may emit a bounded diagnostic.

The initial opcode catalog contains constants, PHI, copy, canonicalization,
statepoints, and the branch, conditional branch, return, throw, and unreachable
terminators. Representations are void, control, fixed integer widths, double,
semantic pointer, and canonical zval. These are semantic values, not machine
register classes. Later PHP opcodes and representations are additive only.

Constants use an immutable pool keyed by value ID. Tagged entries contain
integer or IEEE-754 double bits, payload-free null/boolean values, or symbol IDs
for strings and semantic pointers. Kind/representation combinations are
verified, and no entry contains a host pointer or target encoding.

`signed_integer_bits` permits `i1`, `i8`, `i16`, `i32`, `i64`, or `zval`; fixed
width values occupy the low N bits with higher bits zero, while `zval` uses a
canonical signed 64-bit two's-complement value. `double_bits` permits `double`
or `zval` and stores IEEE-754 binary64 bits. Null, false, true, and string
symbols require `zval`; semantic-pointer symbols require `semantic_pointer`.
Symbol kinds require a valid symbol ID and zero payload. Other kinds require
the invalid symbol ID; payload-free kinds also require zero payload. A
`CONSTANT` instruction's result, constant entry, and value record must agree.

Traversal order carries the minimum control-flow association needed by all
tracks: PHI operand N belongs to predecessor N. An unconditional branch has one
successor, and a conditional branch has true successor 0 and false successor
1. Return, throw, and unreachable have no successors.

W01 catalog order is normative and becomes the exact numeric order for 15
effects, 20 memory domains, 7 general ownership states, 10 ownership actions,
7 predicates, 8 barriers, 12 guard facts, 11 composition rules, and the
frame/safepoint/resume catalogs. Frame-slot ownership stays a separate
six-value catalog because it describes materialized frame custody, not the
general MIR value-lifetime lattice.

The process-local storage binding may contain pointers. Immutable record,
frame-state, source-position, cleanup, continuation, resume, and diagnostic
locations contain IDs and scalar values only. No persistent identity contains
a raw pointer.

A fixed-array fixture host implements the frozen callbacks under
`tests/native/mir/contracts`. It is test-only, has finite capacities, returns
failure without partial mutation, and provides neither execution semantics nor
opcode dispatch.

## Decisions introduced beyond W01

- The 32-bit invalid sentinel, high-bit value namespace, and exact maxima.
- Contract version 1.0 and the same-major/additive-minor compatibility rule.
- The minimal core opcode and target-neutral representation catalogs.
- The tagged target-neutral constant pool and CFG/PHI ordering rules.
- Allocator, immutable view, controlled mutator, and opaque storage boundaries.
- Fixed-size diagnostic messages and bounded process-local sinks.
- The static-array test host and its explicit non-production status.

## Consequences

- W02 tracks can compile and test against one header contract while storage is
  implemented independently.
- IDs and serialized references remain stable across allocator and process
  boundaries.
- Target lowering cannot leak register or instruction details back into MIR.
- Incompatible or unmodeled input is rejected instead of receiving a VM
  fallback meaning.

## Alternatives

Exposing arena structs was rejected because it couples all tracks to storage.
Using pointers as identity was rejected because dumps and persistent metadata
would be process-dependent. A target-specific opcode or representation layer
was rejected because ZNMIR must remain canonical before lowering. Linking a
mock implementation into PHP was rejected because test fixtures must not gain
production semantics.

## Verification impact

The contract checker compares every W01 name and number, rejects duplicate IDs,
invalid sentinel changes, raw pointers in serializable records, and target or
TPDE includes. It compiles every header and the fixture host as C11 and C++20.
