# W06 separation and call-transfer contract

## Separation is a protocol

A separation plan records source storage and payload, reason, uniqueness fact,
requirement, result payload, and whether concrete clone execution is required.

For `required=yes`, source and result payload IDs must differ. For
`required=no`, a no-op result may retain identity. `required=unknown` is
conservative and remains non-codegen-eligible. W06 never allocates or copies a
string or array container; W07 owns that execution semantics.

## Direct-user-call transfers

W05 call records remain unchanged. W06 adds separate v2 transfer records keyed
by stable call-site ID. Parameter modes use a span plus 32-bit ordinals; there
is no 64-bit mask and parameter 65 or 128 is representable.

Each transfer records argument storage, optional reference cell, action, return
storage/reference cell, and return action. By-reference arguments require an
exact lvalue and reference cell.
By-value refcounted arguments require an explicit copy-addref or transfer.

W06 may close call-result ownership, reference-argument transfer, and
refcounted-argument transfer for an exact direct same-script user call. Runtime
binding, call execution, exceptional cleanup, observer integration, internal C
ABI, and container clone execution remain open.

## Direct verification and integrity

The W06 verifier reads the finalized module and value tables directly. It is
deterministic, bounded, and read-only, and fails closed on malformed storage,
payload, reference, alias, ownership, separation, or call-transfer records.
Lowering recomputes deterministic source and module fingerprints before it
returns, so an intervening mutation is rejected without persisting verifier
receipts or registry identifiers in MIR.
