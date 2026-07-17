# ZNMIR canonical text format

`znmir-text-v1` is the deterministic, target-neutral diagnostic serialization
of the frozen ZNMIR contract. Version 1.0 is identified by the first line:

```text
znmir 1.0 module m<ID>
```

The production implementation is a streaming dumper only. The inverse parser
lives under `tests/native/mir/text/`, writes solely to the static-array fixture
host, and is never linked into PHP or a production library. The format is a
diagnostic and verification contract; it is not an OPcache persistence format.

## Lexical grammar

All bytes are printable ASCII (`0x20` through `0x7e`) or line feed (`0x0a`).
Every record, including `end`, has exactly one terminating line feed. Tabs,
carriage returns, blank lines, leading/trailing whitespace, uppercase hex,
decimal leading zeroes, comments, and bytes following `end` are invalid.

The notation below uses quoted literals, `|` for alternatives, and `*` for
repetition. Spaces inside literals are mandatory.

```text
u32       = "0" | nonzero-digit digit*
hex4      = 4 lowercase hexadecimal digits
hex8      = 8 lowercase hexadecimal digits
hex16     = 16 lowercase hexadecimal digits
boolean   = "true" | "false"
scalar    = u32 | "invalid"
id(P)     = P u32 | "invalid"
enum(E)   = a label from catalog E
id-list(P)= "[" (id(P) (", " id(P))*)? "]"
span      = u32 "+" u32
```

The prefixes are `m` module, `f` function, `b` block, `i` instruction, `v`
value, `fs` frame state, `p` source position, `r` resume entry, and `s` symbol.
The word `invalid` denotes the unique `UINT32_MAX` ID sentinel. For diagnostic
dumps of malformed IR, unknown enum values are displayed as `invalid(N)`; they
are never coerced to a known meaning or omitted. This form is deliberately
rejected by the strict parser because it is outside the valid grammar.

## Record grammar

The complete version 1.0 stream is `header`, zero or more records in the
section order below, and `end`. Within ID-keyed sections records are ascending
by unsigned ID. Equal malformed IDs retain view order so invalid IR remains
inspectable. Frame-slot, root, cleanup, predecessor, successor, and operand
orders are semantic and remain exactly as supplied by the view.

```text
header = "znmir 1.0 module " id("m") LF

function = "function " id("f") " symbol " id("s")
           " entry " id("b") " flags 0x" hex8 LF

block = "block " id("b") " function " id("f")
        " predecessors " id-list("b")
        " successors " id-list("b") LF

value = "value " id("v") " representation " enum(representation)
        " ownership " enum(ownership-state) LF

constant = "constant " id("v") " representation " enum(representation)
           " kind " enum(constant-kind) " payload 0x" hex16
           " symbol " id("s") LF

source = "source " id("p") " file " id("s") " line " u32
         " columns " u32 ":" u32 LF

slot = "slot " u32 " value " id("v") " index " scalar
       " kind " enum(frame-slot-kind)
       " representation " enum(frame-slot-representation)
       " materialization " enum(materialization)
       " ownership " enum(frame-slot-ownership)
       " rooted " boolean " cleanup " boolean LF

root = "root " u32 LF

cleanup = "cleanup " u32 " action " enum(cleanup-action)
          " state " enum(cleanup-state) LF

continuation = enum(continuation-kind) ":" id("fs") ":" scalar

frame = "frame " id("fs") " function " id("f")
        " parent " id("fs") " function-kind " enum(function-kind)
        " opline " scalar " phase " enum(opline-phase)
        " slots " span " roots " span " cleanups " span
        " return " continuation " exception " continuation
        " bailout " continuation
        " suspend " enum(suspend-kind) ":" scalar
        " code-version " scalar
        " resume " boolean ":" enum(resume-entry-kind) ":" id("r")
        ":" scalar ":" scalar
        " safepoint " enum(safepoint-class)
        " canonical " boolean LF

instruction = "instruction " id("i") " block " id("b")
              " opcode " enum(opcode)
              " representation " enum(representation)
              " result " id("v") " operands " id-list("v")
              " effects 0x" hex4 " reads 0x" hex8
              " writes 0x" hex8 " barriers 0x" hex2
              " ownership-actions 0x" hex4
              " frame " id("fs") " source " id("p") LF

stream = header function* block* value* constant* source* slot* root*
         cleanup* frame* instruction* "end" LF
```

Enum labels and numeric values are exactly those in the frozen catalog macros
in `zend_mir_opcodes.h`, `zend_mir_effects.h`, and
`zend_mir_frame_state.h`. Masks use the frozen bit numbering: effects are four
hex digits, memory domains eight, barriers two, and ownership actions four.
Constant payloads always use 16 digits, including IEEE-754 bit patterns.

## Canonicalization and streaming behavior

The dumper performs no allocation, locale conversion, floating-point printing,
address formatting, timestamping, PID lookup, target lookup, or hash iteration.
It emits directly through `zend_mir_text_writer`. Any callback rejection stops
the stream immediately, returns `false`, and attempts one bounded
`INVALID_TEXT` diagnostic. Consumers must discard the partial byte stream.

Malformed view access is emitted as an explicit `invalid KIND index N` line
where a record cannot be obtained. Such diagnostic-only output, including
`invalid(N)` enum values, is deliberately not accepted by the strict test
parser. Nullable invalid IDs remain part of the canonical grammar.

## Test parser limits

The test-only parser is iterative and has no recursive grammar production.
It fails closed before publishing a usable fixture when any limit is exceeded:

| Resource | Hard limit |
|---|---:|
| input bytes | 1,048,576 |
| lines | 4,096 |
| bytes per line | 4,096 |
| items per list | 256 |
| grammar nesting depth | 1 |
| diagnostics per parse | 1 of a maximum contract budget of 16 |
| functions / blocks | fixture-host capacities (8 / 32) |
| values / constants / instructions | fixture-host capacities (128 each) |
| frames / sources | fixture-host capacities (32 / 64) |
| slots / roots / cleanups / operands / edges | fixture-host capacities |

Errors carry an error code, zero-based byte offset, and one-based line and
column. Overflow, duplicate IDs, inconsistent CFG lists, dangling references,
invalid bytes, truncation, noncanonical order, and trailing data are rejected.

## Golden digests and fuzzing

The golden corpus covers linear, diamond, loop, ownership, exception
statepoint, and nested-frame forms. `tests/native/mir/golden/SHA256SUMS` freezes
SHA-256 over exact bytes. The harness checks repeated dumps, different writer
chunk sizes, reversed nonsemantic fixture storage, and parse/dump roundtrips.

The standard-library mutation runner uses xorshift mutations with the required
seed `20260717`, bounded buffers, 5,000 mandatory cases, and a subprocess
timeout. `mir_text_fuzzer.c` is an optional clang/libFuzzer entry and remains in
the test tree.
