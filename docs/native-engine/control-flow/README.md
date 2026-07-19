# W04 source-backed control flow

W04 lowers the proof-closed subset in `w04-opcode-profile.json` from compiled
Zend source through SSA into canonical ZNMIR. It accepts reducible control flow,
preserves Zend successor and predecessor order, lowers supported PHI nodes, and
publishes a module only after Stage 1, Stage 2, and the source-backed Stage 3
verifier have all passed.

The `native_mir_test` extension remains default-off and is the only compile/dump
bridge. Its W04 production sources are listed in `w04-source-files.json`;
`ext/native_mir_test/config.m4` must match that list exactly. No W04 path
executes MIR, calls a VM opcode handler, or activates TPDE.

The corpus contains 12 accepted and 12 rejected programs. Accepted fixtures
produce checked-in timestamp-free goldens under
`tests/native/control-flow/integration/goldens/`. Rejected fixtures must return
their exact MIRL code and later-wave classification without a partial module.
The differential channel executes the same PHP programs separately with the
reference and candidate binaries and compares stdout, stderr, and termination
byte-for-byte.

Run the static integration checks with:

```sh
python3 scripts/native/control-flow/validate-w04.py --check
python3 scripts/native/control-flow/test-w04.py --self-test
python3 -m unittest discover -s tests/native/control-flow -p 'test_*.py' -v
```

Run the real gate with explicit, distinct binaries:

```sh
python3 scripts/native/control-flow/test-w04.py \
  --reference-php /absolute/path/to/reference/php \
  --candidate-php /absolute/path/to/w04/php
```

ASan and UBSan use the same command plus `--sanitizer address` or
`--sanitizer undefined`. The full Linux hard gate also builds debug NTS and
ZTS, runs 20,000 cases with seed `20260719`, repeats calls 1 through 10, and
checks arena chunk sizes and OPcache on/off determinism. The timestamp-free
summary is `w04-coverage-report.json`.
