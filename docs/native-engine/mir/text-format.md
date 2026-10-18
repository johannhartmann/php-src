# ZNMIR canonical text format

`znmir-text-v1` (`ZEND_MIR_TEXT_FORMAT_NAME` in
`Zend/Native/MIR/Text/zend_mir_dump.h`) is the deterministic, target-neutral
diagnostic serialization of a ZNMIR module. Version 1.0 is identified by the
first line:

```text
znmir 1.0 module m<ID>
```

The production implementation is the streaming dumper `zend_mir_dump_text()`
(`Zend/Native/MIR/Text/zend_mir_dump.c`) only. The inverse parser lives under
`tests/native/mir/text/`, writes solely to the static-array fixture host, and
is never linked into PHP or a production library. The format is a diagnostic
and verification contract; it is not an OPcache persistence format.

## Lexical grammar

All bytes are printable ASCII (`0x20` through `0x7e`) or line feed (`0x0a`).
Every record, including `end`, has exactly one terminating line feed. Tabs,
carriage returns, blank lines, leading/trailing whitespace, uppercase hex,
decimal leading zeroes, comments, and bytes following `end` are invalid.

The notation below uses quoted literals, `|` for alternatives, `?` for an
optional part, and `*` for repetition. Spaces inside literals are mandatory.

```text
u32       = "0" | nonzero-digit digit*
i64       = "0" | "-"? nonzero-digit digit*   (signed 64-bit range)
hex2      = 2 lowercase hexadecimal digits
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

The core prefixes are `m` module, `f` function, `b` block, `i` instruction,
`v` value, `vf` value fact, `fs` frame state, `p` source position, `r` resume
entry, and `s` symbol. The call and value-model records below add `ct` call
target, `ca` call argument, `cc` call continuation, `cs` call site, `sc`
source call site, `oa` op array, `vp` payload, `vs` storage, `rc` reference
cell, `ac` alias class, `oe` ownership event, and `sp` separation plan.

The word `invalid` denotes the unique `UINT32_MAX` ID sentinel. For diagnostic
dumps of malformed IR, unknown enum values are displayed as `invalid(N)`; they
are never coerced to a known meaning or omitted. This form is deliberately
rejected by the strict parser because it is outside the valid grammar.

## Core records

The stream is `header`, zero or more records in the section order below, and
`end`. Within ID-keyed sections (functions, blocks, values, constants, facts,
sources, frames, instructions) records are ascending by unsigned ID; equal
malformed IDs retain view order so invalid IR remains inspectable. Slots,
roots and cleanups are emitted in pool order. Frame-slot, root, cleanup,
predecessor, successor, and operand orders are semantic and remain exactly as
supplied by the view.

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

fact = "fact " id("vf") " value " id("v") " type " scalar-type
       " flags 0x" hex8 " range " (i64 ":" i64 | "none")
       " provenance " fact-provenance " source " id("p") LF

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

stream = header function* block* value* constant* fact* source* slot*
         root* cleanup* frame* instruction* call-model? value-model?
         "end" LF
```

`fact` records appear only when the view exposes value facts. The range is
printed when `ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE` is set, otherwise `none`.
`scalar-type` is `null`, `i1`, `i64` or `f64`; `fact-provenance` is `ssa`,
`literal`, `range_analysis`, `type_analysis` or `contract`
(`zend_mir_scalar.h`).

Enum labels and numeric values are exactly those in the catalog macros in
`zend_mir_opcodes.h` (all opcode catalogs, representations and constant
kinds), `zend_mir_effects.h`, and `zend_mir_frame_state.h`. Masks use the
catalog bit numbering: effects are four hex digits, memory domains eight,
barriers two, and ownership actions four. Constant payloads always use 16
digits, including IEEE-754 bit patterns.

## Call and value-model records

A module built by the production core also exposes a call view and a value
view. Their records follow the instructions, in view order, and are part of
the canonical bytes (`zend_mir_call_model.c` digests the dump). Enum fields
other than the three labeled ones are printed as their numeric values from
`zend_mir_call.h` and `zend_mir_values.h`.

```text
call-model = call-target* call-argument* call-continuation* call-site*

call-target = "call-target " id("ct") " kind " call-target-kind
              " symbol " id("s") " op-array " id("oa")
              " args " u32 " required " u32 " flags 0x" hex8 LF

call-argument = "call-argument " id("ca") " site " id("cs")
                " ordinal " u32 " value " id("v")
                " ownership " "borrowed_scalar" LF

call-continuation = "call-continuation " id("cc") " site " id("cs")
                    " kind " call-continuation-kind " block " id("b") LF

call-frame(L) = " " L " frame " id("fs") " function " id("f")
                " symbol " id("s") " op-array " id("oa")
                " slots " span " pending " scalar

call-site = "call-site " id("cs") " source " id("sc")
            " instruction " id("i") " target " id("ct")
            " arguments " span " result " id("v")
            " result-operand " u32 ":" u32 ":" u32 ":" u32
            call-frame("caller") call-frame("callee")
            " continuations " span " effects 0x" hex4 " reads 0x" hex8
            " writes 0x" hex8 " barriers 0x" hex2 LF

value-model = value-payload* value-storage* value-model-flags
              value-location* suspend-live* reference-cell*
              alias-relation* ownership-event* separation-plan*
              call-transfer*

value-payload = "value-payload " id("vp") " category " u32
                " refcount " u32 " cleanup " boolean LF

value-storage = "value-storage " id("vs") " kind " u32 " state " u32
                " category " u32 " payload " id("vp")
                " reference " id("rc") " indirect " id("vs") LF

value-model-flags = "value-model-flags " u32 LF

value-location = "value-location " id("v") " frame-storage " u32
                 (" frame-argument " u32)? " category " u32
                 " refcount " u32 " alias-observable " boolean LF

suspend-live = "suspend-live " u32 " value " id("v")
               " frame-storage " u32 LF

reference-cell = "reference-cell " id("rc") " storage " id("vs")
                 " alias " id("ac") " source " id("p")
                 " ownership " u32 " cleanup " boolean LF

alias-relation = "alias-relation " id("ac") " " id("ac")
                 " relation " u32 " proof " u32 LF

ownership-event = "ownership-event " id("oe") " source " id("vs")
                  " target " id("vs") " payload " id("vp")
                  " action " u32 " transition " u32 "->" u32
                  " cleanup " boolean LF

separation-plan = "separation-plan " id("sp")
                  " source-payload " id("vp") " source-storage " id("vs")
                  " reason " u32 " uniqueness " u32 " required " u32
                  " result " id("vp") " clone-execution " boolean LF

call-transfer = "call-transfer " id("cs")
                " parameter-modes " span " argument " u32
                " storage " id("vs") " argument-reference " id("rc")
                " action " u32 " return-storage " id("vs")
                " return-reference " id("rc") " return-action " u32 LF
```

`call-target-kind` is `direct_user`, `direct_internal`, `method_user` or
`dynamic`; `call-continuation-kind` is `normal`, `exception_debt`,
`bailout_reentry_debt` or `observer_debt`. The test parser accepts only the
core records; the fixture host exposes no call or value view.

## Canonicalization and streaming behavior

The dumper performs no allocation, locale conversion, floating-point printing,
address formatting, timestamping, PID lookup, target lookup, or hash iteration.
It emits directly through `zend_mir_text_writer`. Any callback rejection stops
the stream immediately, returns `false`, and attempts one bounded
`INVALID_TEXT` diagnostic. An incomplete view, an incompatible contract
version, or a table above its hard count limit fails the same way. Consumers
must discard the partial byte stream.

Malformed view access is emitted as an explicit `invalid KIND index N` line
where a record cannot be obtained. Such diagnostic-only output, including
`invalid(N)` enum values, is deliberately not accepted by the strict test
parser. Nullable invalid IDs remain part of the canonical grammar.

## Test parser limits

The test-only parser is iterative and has no recursive grammar production.
It fails closed before publishing a usable fixture when any limit is exceeded
(`tests/native/mir/text/mir_test_parser.h`,
`tests/native/mir/contracts/fixture_host.h`):

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

The golden corpus in `tests/native/mir/golden/` covers linear, diamond, loop,
ownership, exception-statepoint, and nested-frame forms; `SHA256SUMS` there
freezes SHA-256 over the exact bytes. `tests/native/mir/text/run_text_tests.py`
checks repeated dumps, different writer chunk sizes, reversed nonsemantic
fixture storage, parse/dump roundtrips, and the digests.

`tests/native/mir/fuzz/run_mutation_fuzz.py` takes an explicit `--seed` and
`--cases` count and runs xorshift mutations of the golden fixtures with
bounded buffers and a subprocess timeout; every mutated input the parser
accepts must dump and parse again. `mir_text_fuzzer.c` is an optional
clang/libFuzzer entry and remains in the test tree.
