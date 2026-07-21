# W05 direct user-call model

W05 models, but does not execute, exact direct calls to user functions declared
in the same compiled script. The accepted slice includes complete reachable
INIT/SEND/DO sequences, ordered scalable parameter-mode records, exact direct
self-calls, default-bearing targets when every argument is supplied, and
compile-normalized named calls whose final target and ordinal mapping are fully
static. Runtime named containers, missing defaults, unpacking, by-reference
transfers, dynamic targets, and refcounted transfers remain deferred.

The modeled result contains pointer-free call targets, arguments, caller and
callee frames, continuations, and the capability/debt boundary. It is
always `codegen_eligible=false`. Runtime binding, exceptions and bailout
reentry, observers, result ownership, internal handlers, and C-ABI
interoperability remain explicit later-wave debts. There is no MIR execution,
VM fallback, target emission, or TPDE integration in W05.

## Running the tests

Use explicit absolute paths for both PHP binaries:

```sh
python3 scripts/native/calls/check-contract.py --check
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

`validate-w05.py` checks only technical state: generated source wiring, live
opcode decisions, exact accepted/deferred corpus classification, raw MIR
goldens, and the absence of runtime or target-code fallbacks. CI runs these
checks directly; it does not maintain a parallel evidence format.
