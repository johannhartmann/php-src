# ZNMIR core and verification

ZNMIR is the architecture-independent PHP MIR the native compiler lowers Zend
op arrays into before TPDE emits machine code. The core lives in
`Zend/Native/MIR/`; `zend_mir.h` holds the root records and callback
interfaces exchanged between components.

- `Core`: arena-backed functions, blocks, values, constants, instructions and
  stable IDs;
- `CFG`: transactional edges, PHI maintenance, critical-edge splitting and
  dominance;
- `Semantics`: the generated effect, alias, ownership and cleanup model;
- `Frame`: frame-state interning and source maps;
- `Text`: the canonical text dump ([format](text-format.md));
- `Verify`: fail-closed identity, CFG, dominance, semantics, frame and source
  verification.

Verification never repairs IR. Repeated dumps of the same module are
byte-identical, and negative cases assert a stable `MIRV` code and location.

## Commands

```sh
python3 scripts/native/mir/test-mir.py --cc "${CC:-cc}"
python3 scripts/native/mir/test-mir.py --cc "${CC:-cc}" --sanitizer address
python3 scripts/native/mir/validate-mir.py --check
python3 tests/native/mir/fuzz/run_mutation_fuzz.py --seed 20260717 --cases 20000
```

`test-mir.py` builds the MIR sources standalone with the unit and integration
tests; `validate-mir.py --check` adds the contract check, the semantic-ID
generator check and the architecture-leak scan.
