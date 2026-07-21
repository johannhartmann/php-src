# W06 value and reference model

W06 is a target-neutral, modeling-only extension of ZNMIR. It records storage,
payload, reference, indirect, ownership, alias, separation, and direct-user-call
transfer facts. It executes none of them.

## Identity and storage

Every persistent identity is a stable 32-bit index. `ZEND_MIR_ID_INVALID` is
the only invalid value. Process addresses, `zval *`, `zend_reference *`, and
target registers are never identities.

A storage record has exactly one kind and state. A `reference` state names a
reference cell; an `indirect` state names a target storage. These are not
interchangeable:

```text
reference cell -> shared mutable payload storage
indirect slot  -> another storage slot
```

Cycles in indirect targets are invalid. A reference cell without a valid
reference-payload storage is invalid.

## Payload and ownership

Payload categories are explicit. W06 can model non-refcounted scalars and
refcounted strings when all transitions are known. Concrete containers,
objects, resources, and unknown categories remain deferred.

No numeric refcount is persistent. The only abstract refcount states are
`immortal`, `unique`, `shared`, and `unknown`. Ownership events use exactly:
`borrow`, `copy_addref`, `move`, `release`, `transfer_to_callee`, and
`transfer_from_callee`.

A potentially final release records a cleanup obligation and leaves
`destructor_exception_cleanup` open. Reading a moved or released value,
double release, or destruction of a borrowed value is invalid.

## Alias and CFG merge

Alias relations are `must_alias`, `may_alias`, and `no_alias`. Unknown or
conflicting provenance becomes `may_alias`. `no_alias` requires a nonzero
proof ID. Equal reference-cell IDs are `must_alias`.

PHI and control-flow merges are conservative. Unique plus shared becomes
shared; unknown remains unknown. Direct, reference, and indirect storage states
never silently merge. An explicit canonicalization record is required.

## Failure atomicity

The complete source inventory and immutable value plan are validated before
the first MIR write. OOM, overflow, malformed identity, unsupported category,
or incomplete transition publishes no partial value records and no module.
