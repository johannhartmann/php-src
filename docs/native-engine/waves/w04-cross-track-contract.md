# W04 cross-track contract

W04 starts from `01e51448e2bc9423d7dc1254ae5e4d34fc236eb4`. Both specialist
branches must use that exact commit as their actual and declared base. The
integration branch alone advances to the wave-pin commit after this contract
base.

## Parallel tracks

W04 has exactly two parallel specialist tracks:

- `W04-A-production-control-flow` owns production source-backed control-flow
  lowering and its isolated unit tests.
- `W04-B-control-flow-evidence` owns the test-extension bridge, corpus, fuzz,
  profiles, and differential evidence.

Their path sets are generated from
`docs/native-engine/control-flow/w04-ownership.json` and are disjoint. Neither
specialist may edit root contracts, profiles, ownership data, wave files, or
integration-reserved paths.

## Immutable root and provider boundaries

The H headers, diagnostics, verifier declarations, source contracts, generated
opcode profile, source manifest, ownership manifest, and contract tests are
read-only for both specialists. Each track consumes the frozen interfaces and
must not manually integrate the other track's providers, bridge code, or tests.

## Integration

W04-I merges the two real specialist heads whose histories start exactly at H.
It must not substitute reconstructed commits or resolve ownership conflicts
manually. An overlap, foreign path, root change, or integration-path change in
a specialist head is a gate failure and must be repaired in the owning branch.

`ext/native_mir_test/config.m4`, `.github/workflows/native-w04.yml`, the W04
validation and result scripts, the integration corpus, the coverage report,
and `docs/native-engine/waves/status.md` remain reserved for W04-I. Production
source wiring in `config.m4` is generated from the frozen W04 source manifest.
