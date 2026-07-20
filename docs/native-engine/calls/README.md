# W05 direct user-call model

W05 models, but does not execute, exact direct calls to user functions declared
in the same compiled script. The accepted slice includes complete reachable
INIT/SEND/DO sequences, ordered scalable parameter-mode records, exact direct
self-calls, default-bearing targets when every argument is supplied, and
compile-normalized named calls whose final target and ordinal mapping are fully
static. Runtime named containers, missing defaults, unpacking, by-reference
transfers, dynamic targets, and refcounted transfers remain deferred.

The modeled result contains pointer-free call targets, arguments, caller and
callee frames, continuations, receipts, and the capability/debt boundary. It is
always `codegen_eligible=false`. Runtime binding, exceptions and bailout
reentry, observers, result ownership, internal handlers, and C-ABI
interoperability remain explicit later-wave debts. There is no MIR execution,
VM fallback, target emission, or TPDE integration in W05.

## Reproducing the gate

Use explicit absolute paths for both PHP binaries:

```sh
python3 scripts/native/calls/check-contract.py --check
python3 scripts/native/calls/check-phases.py --check
python3 scripts/native/calls/validate-w05.py --check
python3 scripts/native/calls/test-w05.py \
  --reference-php /absolute/path/to/reference/php \
  --candidate-php /absolute/path/to/w05/php
TEST_PHP_EXECUTABLE=/absolute/path/to/w05/php \
  python3 -m unittest discover -s tests/native/calls -p 'test_*.py' -v
TEST_PHP_EXECUTABLE=/absolute/path/to/w05/php \
  python3 tests/native/calls/fuzz/run_fuzz.py \
    --seed 20260719 --cases 20000
```

`test-w05.py` first compares ordinary PHP execution byte-for-byte and then
separately compiles source through Zend SSA and the W05 call model. Accepted
cases must pass all prerequisite verifiers and match the raw canonical
goldens. Deferred cases must return their exact MIRL code and later wave
without publishing MIR. No normalization is applied.

The timestamp-free coverage projection is
`w05-coverage-report.json`. Its opcode counts are live-derived, its
reclassification list is complete, and it binds the approved independent
review manifest and the global native source manifest. `validate-w05.py
--check` rejects any drift.

## Durable seal

Each required command is captured with `seal-w05.py run`; its committed v2
summary contains normalized repository-relative arguments, independent
stdout/stderr digests, duration, and a digest-bound raw log below the external
artifact root. The gate commit contains every summary and the two unchanged
read-only review JSON files.

After QG has passed, `seal-w05.py seal --subject <QG> --write ...` archives the
v1 receipt and creates the v2 receipt. It binds the exact QH/QP/QM/QG phase
chain, W04 dependency receipt, profiles, reviews, coverage, summaries, and
reference/candidate binary manifests. The receipt subject is QG, not the
containing QS commit. The task result therefore uses `tested_head_commit` and
a receipt digest while leaving `head_commit` null.

Full verification rehashes every external raw log and binary:

```sh
python3 scripts/native/verify-wave-receipt.py W05 \
  --level full --artifact-root /absolute/path/to/w05-v2-artifacts
```
