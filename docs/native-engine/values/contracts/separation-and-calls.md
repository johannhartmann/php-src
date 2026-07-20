# W06 separation and call-transfer contract

## Separation is a protocol

A separation plan records source storage and payload, reason, uniqueness fact,
requirement, result payload, and the `container_clone_execution` debt.

For `required=yes`, source and result payload IDs must differ. For
`required=no`, a no-op result may retain identity. `required=unknown` is
conservative and remains non-codegen-eligible. W06 never allocates or copies a
string or array container; W07 owns that execution semantics.

## Direct-user-call transfers

W05 call records remain unchanged. W06 adds separate v2 transfer records keyed
by stable call-site ID. Parameter modes use a span plus 32-bit ordinals; there
is no 64-bit mask and parameter 65 or 128 is representable.

Each transfer records argument storage, optional reference cell, action, return
storage/reference cell, return action, and a span of debts resolved for that
callsite. By-reference arguments require an exact lvalue and reference cell.
By-value refcounted arguments require an explicit copy-addref or transfer.

W06 may close call-result ownership, reference-argument transfer, and
refcounted-argument transfer for an exact direct same-script user call. Runtime
binding, call execution, exceptional cleanup, observer integration, internal C
ABI, and container clone execution remain open.

The global registry already names the execution debts `call_execution` and
`internal_c_abi_interop`; W06 reuses those canonical IDs. It does not create
colliding debt aliases named `target_emission` or `internal_c_abi`.

## Verifier receipts

One successful W06 result requires structural, scalar, control-flow, and
value/reference receipts. A call-model receipt is also required when calls are
present. Every receipt binds the same final module and source fingerprints.
The value/reference verifier is deterministic, bounded, read-only, and fails
closed on missing or mismatched receipts.
