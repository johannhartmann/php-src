# ZNMIR core and verification

ZNMIR is the architecture-independent PHP MIR the native compiler lowers Zend
op arrays into before TPDE emits machine code. The core lives in
`Zend/Native/MIR/`; `zend_mir.h` holds the root records and callback
interfaces exchanged between components, and
[`Zend/Native/MIR/README.md`](../../../Zend/Native/MIR/README.md) describes the
header layers and identity rules.

- `Core`: arena-backed module, functions, blocks, values, constants,
  instructions, stable IDs and the read-only view;
- `CFG`: transactional edges, PHI maintenance, critical-edge splitting and
  dominance;
- `ControlFlow`: the process-local map from Zend control flow to MIR blocks
  and its verifier;
- `Scalar`: scalar opcode descriptors, value facts and their verifier;
- `Semantics`: the effect, alias, ownership and cleanup catalog generated
  from the [effect model](../semantics/effects/README.md);
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

`test-mir.py` builds every MIR source with the fixture host and runs the
integration binary and the C11/C++20 header tests (`--sanitizer` takes
`address` or `undefined`). `validate-mir.py --check` adds the
architecture-leak scan, the contract check (`check-contract.py`), the
semantic-catalog generator check (`generate-semantic-ids.py --check`) and the
Python unit tests under `tests/native/mir`.
