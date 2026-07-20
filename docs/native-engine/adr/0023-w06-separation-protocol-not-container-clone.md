# ADR 0023: W06 separation is a protocol, not a container clone

## Status

Accepted for W06.

## Decision

W06 records whether separation is required and why. A plan names source
storage/payload, reason, uniqueness fact, requirement, result payload, and the
container-execution debt. It never performs a string or array clone.

When cloning is required, source and result payload IDs must differ. Unknown
uniqueness remains an unknown requirement and is never treated as no-op.
Cleanup and destructor effects are debts, not hidden runtime actions.

W05 direct-user-call records are not modified. W06 adds pointer-free transfer
records keyed by call-site ID with scalable parameter-mode spans and explicit
argument/result ownership actions.

## Consequences

Container clone execution and concrete string/array semantics remain W07 work.
Observable destructor and exceptional cleanup remain W09 work. Runtime
reference binding and dynamic symbol-table aliasing remain W10 work. W06 stays
model-only and `codegen_eligible=false`.
