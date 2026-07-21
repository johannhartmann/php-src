# ADR 0022: W06 storage, reference, and alias model

## Status

Accepted for W06.

## Decision

ZNMIR represents a PHP value through separate pointer-free identities for
storage, payload, reference cell, and alias class. Indirect storage is a link
to another storage ID, while a reference cell owns a shared mutable payload
storage ID. Neither representation stores a runtime address.

Refcounting is abstract. Persistent records contain only immortal, unique,
shared, or unknown state and explicit ownership actions. Alias knowledge is
must, may, or proof-backed none. Unknown information always weakens to may
alias. PHI merges conservatively join ownership and alias facts.

W06 records successful value/reference verification directly in the lowering
result. The verifier recomputes the final source and module fingerprints before
the module is published; it does not create persistent receipt or capability
records.

## Consequences

W06 can model local reference identity and refcount transfers without claiming
runtime reference binding, destructor behavior, or execution. Existing W01-W05
IDs, layouts, dumps, guarantees, and verifier meanings remain unchanged.
