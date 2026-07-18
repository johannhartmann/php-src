# ZNMIR W02 integration gate

W02 provides the standalone, architecture-independent ZNMIR core and its
Stage-1 verifier. It does not activate MIR from the PHP executor, OPcache, the
native build, or any product binary. Target lowering and VM fallback are
outside this wave.

## Component boundary

The frozen root records and callback interfaces in `Zend/Native/MIR/zend_mir.h`
are the only cross-component exchange format. The component directories have
the following responsibilities:

- `Core`: arena-backed functions, blocks, values, constants, instructions, and
  stable IDs;
- `CFG`: transactional edges, PHI maintenance, critical-edge splitting, and
  dominance;
- `Semantics`: the generated W01 effect, alias, ownership, and cleanup model;
- `Frame`: immutable frame-state interning and source maps;
- `Text`: canonical, streaming `znmir-text-v1` dumps;
- `Verify`: fail-closed Stage-1 identity, CFG, dominance, semantics, frame, and
  source validation.

The integration harness uses the frozen view contract as a test-only adapter.
It links every production C source in one binary, exercises concrete Core and
CFG storage directly, and supplies complete synthetic records for cross-track
verification and dumping. No adapter is linked into PHP.

## Determinism and diagnostics

Positive programs are verified before dumping. Their input is snapshotted to
prove that verification is non-mutating. Repeated dumps, one-byte writer
chunking, reversed non-semantic record storage, and Core modules built with
different arena chunk sizes must produce identical bytes and hashes.

Negative programs assert both their stable `MIRV` code and the exact function,
block, and instruction location. Verification failures are observable and do
not repair or publish altered IR.

The timestamp-free inventory and coverage projection is
`w02-coverage-report.json`. Its contents are generated from the checked-in
source and golden fixtures; `--check` rejects stale bytes.

## Commands

Run the standalone integration gate:

```sh
python3 scripts/native/mir/test-w02.py --cc "${CC:-cc}"
python3 scripts/native/mir/test-w02.py --cc "${CC:-cc}" --sanitizer address
python3 scripts/native/mir/test-w02.py --cc "${CC:-cc}" --sanitizer undefined
```

Regenerate and validate the deterministic report:

```sh
python3 scripts/native/mir/validate-w02.py --write-report
python3 scripts/native/mir/validate-w02.py --check
```

Run the fixed-seed mutation campaign:

```sh
python3 tests/native/mir/fuzz/run_mutation_fuzz.py \
  --seed 20260717 --cases 20000
```

`validate-w02.py --check` also runs the W01 validator, frozen contract check,
semantic-ID generator check, strict C11/C++20 integration build, the complete
MIR unit discovery, architecture-leak scan, and deterministic report check.
