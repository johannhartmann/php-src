# W03 cross-track contract

W03 starts from `e310e29c4e71c1ac5f4cda526ce88b86d8e82c91`. Every specialist branch
must use that commit as its actual and declared base. W03-0 itself is not a
wave-gate result.

## Frozen root contracts

The 1.2 headers, profile, generator, checker, and contract fixtures are frozen
at the base. Track F exclusively owns any necessary changes to the root MIR
headers and owns scalar MIR construction, canonical text support, and stage-2
verification. Tracks A-E and G consume those interfaces and must not edit them.

## Provider integration

Tracks A-E contribute implementations only in their owned Lowering subtree.
Provider claims are merged through Track A's deterministic registry. A provider
may claim only opcodes assigned to its track by
`w03-opcode-profile.json`; duplicate claims fail the registry and the gate.
Insertion and link order must not affect provider enumeration or MIR output.

## Deferred semantics

Missing or contradictory proofs never select a permissive implementation.
Deferred profile entries produce the stable W04, W05, W06, or later-wave
diagnostic assigned by the profile and return no module. No track may add a VM
fallback, runtime helper shortcut, target lowering, or unprofiled opcode.

## Merge order

Merge A, B, C, D, E, F, and G independently onto the pinned base, resolving no
cross-track ownership by opportunistic edits. The integration gate then merges
A through G in that order, runs the complete W01/W02/W03 suite, verifies
failure atomicity and deterministic dumps, and alone records the W03 gate
result.
