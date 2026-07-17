# ADR 0012: ZNMIR text, diagnostics, and verification

## Status

Accepted on 2026-07-17.

## Context

W02 text and verifier tracks must produce identical observations over either
the test fixture or production storage. Their output, rejection behavior, and
diagnostic limits must therefore be fixed before either implementation starts.

## Decision

The canonical text form is UTF-8, LF-terminated, and emitted solely through a
`zend_mir_view`. It starts with `znmir 1.0 module m<ID>`. Functions, blocks,
frame states, and instructions follow in ascending numeric ID order. References
use typed decimal IDs (`f`, `b`, `i`, `v`, `fs`, `p`); the invalid sentinel is
spelled `invalid`. Catalog values use their lowercase contract labels. Masks
use fixed-width lowercase hexadecimal: four digits for effects and ownership
actions, eight for domains, and two for barriers.

Lists use comma-space separators inside square brackets and preserve semantic
operand or successor order. Empty lists are `[]`. Each field appears exactly
once in the order defined by the corresponding immutable record. No address,
allocator detail, target feature, machine offset, or incidental traversal order
is printable. The parser accepts only this versioned canonical form: unknown,
duplicate, missing, overflowing, out-of-range, or trailing data is rejected.
Dump-parse-dump must be byte-identical.

Version 1.0 uses these line records, in the shown field order. `<id>` is an
unsigned decimal integer, `<bool>` is `true` or `false`, `<span>` is
`<offset>+<count>`, and a typed ID also accepts `invalid` where its field is
nullable. Continuations are `<kind>:<frame-id>:<opline>` and resume references
are `<allowed>:<entry-kind>:<resume-id>:<code-version-id>:<opline>`.

```text
znmir 1.0 module m<id>
function f<id> symbol s<id> entry b<id> flags 0x<8 hex>
block b<id> function f<id> predecessors [<block-ids>] successors [<block-ids>]
value v<id> representation <label> ownership <label>
constant v<id> representation <label> kind <label> payload 0x<16 hex> symbol s<id>
source p<id> file s<id> line <id> columns <id>:<id>
slot <id> value v<id> index <id> kind <label> representation <label> materialization <label> ownership <label> rooted <bool> cleanup <bool>
root <slot-id>
cleanup <slot-id> action <label> state <label>
frame fs<id> function f<id> parent fs<id> function-kind <label> opline <id> phase <label> slots <span> roots <span> cleanups <span> return <continuation> exception <continuation> bailout <continuation> suspend <label>:<state-id> code-version <id> resume <resume-reference> safepoint <label> canonical <bool>
instruction i<id> block b<id> opcode <label> representation <label> result v<id> operands [<value-ids>] effects 0x<4 hex> reads 0x<8 hex> writes 0x<8 hex> barriers 0x<2 hex> ownership-actions 0x<4 hex> frame fs<id> source p<id>
end
```

Functions, blocks, values, constants, source positions, frame states, and
instructions are sorted by their numeric ID. Slot, root, and cleanup records
retain pool index order because frame spans address those pools. Record kinds
occur in the order above. Exactly one ASCII space separates tokens, no line has
trailing whitespace, and the file ends after the newline following `end`.

For payload-free constants, `payload` is all zeroes and `symbol` is `invalid`.
Integer and double constants use `payload`; string and semantic-pointer
constants use `symbol` and an all-zero payload. PHI operand N is paired with
predecessor N. Conditional successor 0 is true and successor 1 is false; an
unconditional branch has only successor 0.

Diagnostics contain a stable code, severity, ID-based location, and a message
of at most 191 bytes plus NUL. A sink invokes at most its configured `limit`
callbacks. Exhausting the limit returns failure and does not silently discard
an error while reporting success. Diagnostic order is verifier stage, then
function, block, instruction, and frame-state ID; messages may add context but
must not replace the stable code or location oracle.

Stage-one verification consumes only `zend_mir_view` and fails closed. It
checks at least contract compatibility; unique and valid IDs; referential
integrity; function/block membership; entry blocks; terminator placement and
successor arity; predecessor/successor symmetry; PHI placement, input count,
and predecessor association; value definition/use and representation; effect,
domain, ownership, and barrier ranges; and frame-state/source/resume reference
integrity. Arithmetic used for counts, spans, and diagnostics is checked before
addition or multiplication. No malformed module may proceed to lowering.

## Decisions introduced beyond W01

- The canonical text header, typed decimal IDs, mask widths, ordering, and
  strict byte-stable round-trip rule.
- Strict parser rejection rather than permissive normalization.
- Stable diagnostic code/location as the test oracle and the 192-byte message
  capacity.
- Bounded sink behavior and deterministic diagnostic ordering.
- The minimum scope and fail-closed ordering of stage-one verification.

## Consequences

- Text and verifier tracks can be tested against the fixture host without the
  production arena.
- Dumps are reproducible and safe for semantic diffs.
- Negative tests can assert stable codes and locations without depending on
  prose.
- Later syntax or verifier extensions require an additive contract version and
  corresponding round-trip and rejection tests.

## Alternatives

Printing storage addresses was rejected as nondeterministic and nonpersistent.
A tolerant parser was rejected because normalization can conceal malformed
input. Unbounded diagnostics were rejected because hostile or corrupted graphs
could produce unbounded work. Verifying only during lowering was rejected
because target code must never receive malformed canonical MIR.

## Verification impact

The text track must include golden output, byte-identical round trips, overflow,
unknown-field, duplicate-field, ordering, and trailing-data tests. The verifier
track must cover every invariant with a stable diagnostic code/location and
must prove diagnostic limits are honored.
