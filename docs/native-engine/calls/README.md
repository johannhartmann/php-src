# W05 direct user-call model

W05 models, but does not execute, exact direct calls to user functions declared
in the same compiled script. The accepted slice is deliberately narrow:
positional by-value `null`, `bool`, `long`, and `double` arguments; complete
reachable INIT/SEND/DO sequences; exact argument counts; and unused or
source-proved non-refcounted scalar results.

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

After the gate commit has passed, `seal-w05.py --subject <gate-commit> --write`
binds the gate tree, W04 receipt, profiles, reviews, coverage report, command
logs, goldens, and source manifest. The receipt subject is the gate commit, not
the containing seal commit. The committed task result therefore uses
`tested_head_commit` and a receipt digest while leaving `head_commit` null.

The generic receipt schema frozen by W05-00 has no wave-specific base, pin, or
implementation-head fields. W05 binds those values without extending that
sealed schema: the receipt hashes `w05-review-manifest.json`, and the seal tool
requires that manifest to name the exact H and P commits, two approved reviews
of one implementation head, and an ancestry chain from P through that head to
the gate subject.
