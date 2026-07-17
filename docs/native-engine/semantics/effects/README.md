# Native effect and ownership model

This directory is the normative, machine-readable contract for effects,
aliasing, ownership, cleanup, barriers, and guard invalidation in the native
engine.  The contract is deliberately conservative: an operation that cannot
be mapped to the model is not pure, cannot be reordered, and cannot be
published as native code.

The JSON model is authoritative.  The accompanying documents explain how to
apply it:

- `memory-domains.md` defines the memory partition and alias policy.
- `ownership-and-cleanup.md` defines value states, transfers, PHI merges, and
  cleanup on every exit.
- `guard-stability.md` defines guard proofs and invalidation.
- `code-motion-examples.md` gives concrete allowed and forbidden rewrites.

`effect-model.schema.json` describes the serial form and `effect-model.json`
is the versioned model.  Validate both the structure and the semantic
invariants with:

```sh
python3 scripts/native/semantics/validate-effect-model.py --check
python3 -m unittest discover -s tests/native/semantics/contracts/effects -p 'test_*.py' -v
```

## Decision procedure

For each operation, the frontend must identify every atomic effect, memory
domain, ownership action, predicate, and barrier.  Composition rules then add
transitive behavior.  In particular, a possible destructor transitively adds
PHP reentry, exception, and frame observation.  A transformation is legal
only when its proof discharges every alias, cleanup, guard, and barrier
condition.  Publication additionally requires that every operation and
ownership transition be modeled.

The model describes semantics, not C representation.  No C header, opcode
layout, register convention, or backend detail is part of this contract.
