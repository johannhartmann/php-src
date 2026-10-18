# Native effect and ownership model

This directory is the normative, machine-readable contract for effects,
aliasing, ownership, cleanup, barriers, and guard invalidation in the native
engine.  The contract is deliberately conservative: an operation that cannot
be mapped to the model is not pure, cannot be reordered, and cannot be
published as native code.

The JSON model is authoritative.  The accompanying documents explain how to
apply it:

- [`memory-domains.md`](memory-domains.md) defines the memory partition and
  alias policy.
- [`ownership-and-cleanup.md`](ownership-and-cleanup.md) defines value
  states, transfers, PHI merges, and cleanup on every exit.
- [`guard-stability.md`](guard-stability.md) defines guard proofs and
  invalidation.
- [`code-motion-examples.md`](code-motion-examples.md) gives concrete allowed
  and forbidden rewrites.
- [`../vocabulary.md`](../vocabulary.md) lists the catalog identifiers.

[`effect-model.schema.json`](effect-model.schema.json) describes the serial
form and [`effect-model.json`](effect-model.json) is the versioned model.

## Binding to the code

The model's `catalog` must equal, label for label and in bit order, the
catalog macros in `Zend/Native/MIR/zend_mir_effects.h`
(`scripts/native/mir/check-contract.py`). The MIR semantic tables in
`Zend/Native/MIR/Semantics/zend_mir_semantic_catalog.inc` are generated from
the model by `scripts/native/mir/generate-semantic-ids.py` and record the
model's SHA-256, so any byte change to `effect-model.json` requires
regenerating that file. The MIR verifier
(`Zend/Native/MIR/Verify/zend_mir_verify_semantics.c`) checks every
instruction's effect summary against these tables.

Validate the structure, the semantic invariants and the source references
(each `path:line-range` must exist) with:

```sh
python3 scripts/native/semantics/validate-effect-model.py --check
python3 -m unittest discover -s tests/native/semantics/contracts/effects -p 'test_*.py' -v
python3 scripts/native/mir/generate-semantic-ids.py --check
```

## Decision procedure

For each operation, the frontend must identify every atomic effect, memory
domain, ownership action, predicate, and barrier.  Composition rules then add
transitive behavior.  In particular, a possible destructor transitively adds
PHP reentry, exception, and frame observation.  A transformation is legal
only when its proof discharges every alias, cleanup, guard, and barrier
condition.  Publication additionally requires that every operation and
ownership transition be modeled.

The model describes semantics, not C representation.  No opcode layout,
register convention, or backend detail is part of this contract; the C
catalog macros above only bind its identifiers to numbers.
