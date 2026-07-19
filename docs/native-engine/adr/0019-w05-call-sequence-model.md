# ADR 0019: W05 atomic call-sequence model

## Status

Accepted.

## Decision

W05 models, but does not execute, one deliberately narrow call subset: an
exactly resolved same-script user function introduced by `INIT_FCALL`, supplied
only positional by-value non-refcounted scalar arguments, and completed by a
proved user `DO_UCALL` or `DO_FCALL`. Defaults, named arguments, unpacking,
variadics, placeholders, references, refcounted values, protected regions,
dynamic targets, methods, internal handlers, and unproved results are outside
W05.

The planner scans the complete reachable source sequence before any MIR
mutation. It maintains a LIFO INIT/SEND/DO stack, assigns every SEND to exactly
one active call site, validates the exact target and argument contract, and
publishes either one complete immutable plan or nothing. An opcode provider may
not publish an INIT, SEND, RECV, or DO fragment independently.

Source call-site, target, and argument records use original stable indices and
contain no process pointers. A process-local resolver may inspect Zend function
metadata while constructing the snapshot, but neither the address nor a
`zend_function *` becomes persistent identity.

Zend may normalize a syntactically named argument to a positional SEND when an
exact callee proves its parameter position. W05 must not silently accept that
case. The compiler therefore preserves the syntactic fact with a private
`zend_compile.c` marker in the existing SEND `extended_value` field. The
Native source contract freezes the producer/consumer bit as
`ZEND_MIR_ZEND_SEND_SYNTACTIC_NAMED`, and the source view translates it to the
pointer-free argument flag
`ZEND_MIR_SOURCE_CALL_ARGUMENT_SYNTACTIC_NAMED`. This metadata does not alter
the `zend_op` layout, public headers, public ABI, execution semantics, or
optimizer ownership; it only makes the required W07 deferral source-backed.

Each modeled call records caller state, pending-call slot, callee-entry frame,
ordered arguments, normal continuation, conservative W01 effects and barriers,
an invalid result ID for an unused result or the exact mapped non-refcounted
scalar result ID, and the explicit debts for exception, bailout/reentry,
observer, ownership, runtime binding, and internal C-ABI semantics. The call
instruction occupies the source DO position so surrounding source-backed MIR
instructions retain source-opline order.

The callee body is not lowered in W05. Its entry descriptor therefore carries
an invalid lowered `function_id` and `frame_state_id`, plus the stable
`function_symbol_id` and `op_array_id` of the target declaration. The caller
descriptor retains its valid lowered MIR function and frame-state IDs. A
callee declaration does not enter the generic MIR function/CFG tables until a
later wave lowers its body; the W05 verifier correlates its symbol and op-array
identity with the call target.

## Compatibility

The additive W05 contract is version 1.8. W03 contract 1.2, W04 contract 1.3,
their guarantee masks, their failure-atomic helpers, and all prior stable
diagnostics retain their meanings. W05 adds `CALL_DIRECT_USER` as opcode 41 and
an additive `ZEND_MIR_W05_OPCODE_COUNT` boundary while preserving the historic
W03 scalar `ZEND_MIR_OPCODE_COUNT` boundary. It does not activate a VM
fallback, MIR execution, target lowering, or a public C-ABI.
