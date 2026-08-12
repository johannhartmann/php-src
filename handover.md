# Linux/AMD64 handover for the PHP TPDE native engine

Updated: 2026-08-12

This file is the continuation point for a real Linux x86-64 host. It is not a
fresh implementation plan. Continue from the current `origin/master`, preserve
all existing native-engine contracts, and use the repository's existing build,
PHPT, sanitizer, full-suite, FPM, and benchmark paths.

Do not commit `fixes.md` or `bla.md`. Future updates to `handover.md` also stay
uncommitted unless they are explicitly requested for publication.

## Objective on the Linux host

Complete the target-specific work that could not be validated on Apple ARM:

1. make the existing LinuxX64 backend compile against the current common TPDE
   adaptor and compiler interfaces;
2. align LinuxX64 allocator, direct-call, unwind, boxed-PHI, pointer, and
   by-reference capabilities with the already validated semantics;
3. pass Linux Debug NTS/ZTS, Release NTS, ASan, and UBSan;
4. pass the focused native and OPcache PHPTs without VM fallback;
5. compare the complete php-src PHPT corpus with the pinned reference PHP;
6. pass the real persistent Product FPM path and the existing product
   performance gates.

EncodeGen, Forward Fusion, and general-inliner development may proceed on
Darwin in parallel, but no cross-target V1 parity claim is valid until this
Linux handover is complete.

## Contracts to read before editing

Read these files from the Linux checkout:

1. `AGENTS.md`
2. `CONTRIBUTING.md`
3. `CODING_STANDARDS.md`
4. `fixes.md`
5. `Zend/Native/AGENTS.md`
6. `Zend/Native/TPDE/AGENTS.md`
7. every deeper `AGENTS.md` that applies to a changed path
8. `docs/native-engine/test-command-contract.md`
9. `.github/workflows/native-engine.yml`, especially the `linux-amd64` job

Pinned design references:

```text
php-src reference: 47355da494ba696b1bdb6d10448a225e742bd316
TPDE reference:     338d41890e424b058e2053b6a5787e1348e3dd57
pre-V1 perf base:   654b9c48c24a505c8a567df4fd2e753dff3ae356
W11p perf base:     14dce285da0f849572535f49790b2a63d292685a
```

Hard boundaries:

- no production fallback to the Zend VM;
- no public ABI, persistent-format, or dependency change without an explicit
  compatibility contract and its required tests;
- no changes in `Zend/Native/TPDE/ThirdParty`;
- no new validation, comparator, telemetry, counter, stress-job, or metrics
  infrastructure;
- semantic regressions belong in the existing PHPT suites, primarily
  `ext/native_mir_test/tests`, not in a parallel Python model;
- use serial local builds and tests: `--jobs 1`, `make -j1`, and PHPT `-j1`;
- do not use Docker for this work;
- do not weaken or skip a failing test to make the matrix green.

## Source state to inherit

The handover was written from:

```text
repository: johannhartmann/php-src
branch:     master
HEAD:       d07deba1b8b0295b189cfbb854a241145769c2be
upstream:   origin/master at the same commit
```

Important recent commits, oldest first:

```text
74d363d5379 fix(native): refresh dynamic compiler views
ccaaf877c72 fix(native): guard boxed typed-call arguments
d07deba1b8b test(native): enforce product performance gates
```

The Linux checkout may be newer because Darwin work can continue in parallel.
Do not reset it to the SHA above. Instead, require that these commits are
ancestors of the live `origin/master` and start from the latest remote head:

```bash
git fetch origin
git switch master
git pull --ff-only
git merge-base --is-ancestor d07deba1b8b0295b189cfbb854a241145769c2be HEAD
git status --short
git log -8 --oneline
```

Stop and reconcile with the owner if `master` cannot be fast-forwarded or if
the worktree contains changes you do not understand. Never discard another
session's edits.

## Current remote evidence

GitHub Actions run:

```text
https://github.com/johannhartmann/php-src/actions/runs/31518617784
head: d07deba1b8b0295b189cfbb854a241145769c2be
result: failure
```

Jobs that passed:

```text
darwin-arm64-native-debug-nts
darwin-arm64-native-debug-zts
darwin-arm64-native-ubsan-nts
```

All five Linux profiles failed during `Build and execute native x86-64`,
before Linux PHPT, full-suite, FPM, or performance evidence could be produced:

```text
linux-amd64-native-debug-nts
linux-amd64-native-debug-zts
linux-amd64-native-release-nts
linux-amd64-native-asan-nts
linux-amd64-native-ubsan-nts
```

The `call-model` job passed the performance contract, all native contract
validation, and the pinned reference build, then reached the same LinuxX64
compilation barrier during `Build and test call model`.

Do not hide the other remote failures: Darwin ASan failed during the full-suite
comparison at `Zend/tests/fibers/gh19983.phpt`; Darwin Release later failed its
product-performance step with `cv_assignment_loop` over the runtime threshold.
Those are separate Darwin follow-ups, not proof that the Linux build failure is
acceptable.

## First Linux blocker: compiler type-name ambiguity

The first GCC errors in every Linux build are:

```text
Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp:147:
  error: reference to 'AsmReg' is ambiguous
Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp:165:
  error: reference to 'ValuePart' is ambiguous
Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp:232:
  error: reference to 'ScratchReg' is ambiguous
```

The later undeclared-variable, `int` conversion, TPDE concept, and
`load_address_of_var_reference` errors are mostly cascades from these missing
class-scope type resolutions. Fix and rebuild from the first diagnostic rather
than patching every downstream error independently.

The corresponding Darwin class already disambiguates its inherited compiler
types explicitly. See commit `f4b5780733c` and the top of
`Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_arm64.cpp`:

```cpp
public:
    using AsmReg = typename Base::AsmReg;
    using GenericValuePart = typename Base::GenericValuePart;
    using ScratchReg = typename Base::ScratchReg;
    using ValuePart = typename Base::ValuePart;
    using ValuePartRef = typename Base::ValuePartRef;

private:
```

The narrow first attempt on Linux should mirror that class-scope resolution in
`ZendCompilerX64`, adjusted only if GCC exposes a real target-specific
difference. Do not modify vendored TPDE to work around it. After that single
change, rebuild immediately and reassess the new first compiler diagnostic;
do not assume all Linux work is thereby complete.

## Linux host setup

Use a native x86-64 Linux machine. The CI reference environment is Ubuntu
24.04. Verify the host before doing anything:

```bash
test "$(uname -s)" = Linux
test "$(uname -m)" = x86_64
```

The minimal native harness requires Bash, Git, Python 3.9+, Autoconf,
Autoheader, Bison, Re2c, Make, pkg-config, SHA-256 utilities, `flock`, and a C/C++
compiler. The complete php-src and FPM comparison additionally needs the
packages listed in `.github/actions/apt-x64/action.yml`. Treat that action as
the canonical Ubuntu dependency list; in particular ensure `clang`, `lld`,
`libfcgi-bin`, database clients/servers, image libraries, and the mail/LDAP
test dependencies are installed. The ASan CI profile intentionally omits
`libavif-dev` if it conflicts with that job.

A compact starting set for getting the native build barrier visible is:

```bash
sudo apt-get update
sudo apt-get install -y \
  autoconf bison build-essential clang git libfcgi-bin lld make \
  pkg-config python3 re2c
```

Before the full suite, install the complete canonical list from
`.github/actions/apt-x64/action.yml`; otherwise extension skips and failures are
not comparable to CI.

Use an external artifact root and keep execution serial:

```bash
export NATIVE_WORK_ROOT=/var/tmp/php-native-linux-amd64
export NATIVE_JOBS=1
```

Do not point `NATIVE_WORK_ROOT` at the source tree.

## Validation staircase

Run each stage in order. Stop at the first failure, diagnose it, add a focused
PHPT when the failure is semantic, and only then proceed.

### 1. Debug NTS build

From the repository root:

```bash
scripts/native/build.sh \
  --profile linux-amd64-native-debug-nts \
  --jobs 1 \
  --print-binary
```

The last output line is the candidate CLI path. Set a task-specific variable
to that exact path:

```bash
LINUX_NATIVE_PHP=$(
  scripts/native/build.sh \
    --profile linux-amd64-native-debug-nts \
    --jobs 1 \
    --print-binary | tail -n 1
)
test -x "$LINUX_NATIVE_PHP"
"$LINUX_NATIVE_PHP" -n -v
```

### 2. Smoke and focused execution

```bash
scripts/native/test-smoke.sh \
  --profile linux-amd64-native-debug-nts \
  --jobs 1

TEST_PHP_SRCDIR="$PWD" \
TEST_PHP_EXECUTABLE="$LINUX_NATIVE_PHP" \
  "$LINUX_NATIVE_PHP" -n run-tests.php \
    -n -q -j1 --show-diff --set-timeout 600 \
    ext/native_mir_test/tests/w07_*.phpt \
    ext/native_mir_test/tests/w08_*.phpt \
    ext/native_mir_test/tests/w09_*.phpt \
    ext/native_mir_test/tests/w10_*.phpt \
    ext/native_mir_test/tests/w11_*.phpt \
    ext/native_mir_test/tests/w11p_*.phpt \
    ext/native_mir_test/tests/w12_*.phpt \
    ext/native_mir_test/tests/w14_*.phpt \
    ext/opcache/tests/native_*.phpt
```

This must retain the native assertions in the PHPTs (`vm=0`, `execute_ex=0`,
`handler=0`, and related counters where declared). Correct output alone is not
enough if the test used a VM or handler fallback.

Run the existing cross-target execution comparison after building the pinned
reference PHP described below:

```bash
python3 scripts/native/test-native-execution.py \
  --target linux-amd64-prod \
  --reference "$LINUX_REFERENCE_PHP" \
  --candidate "$LINUX_NATIVE_PHP"
```

For Debug NTS, also exercise the existing Linux persistent runtime path:

```bash
LINUX_NATIVE_SAPI=$(dirname "$(dirname "$LINUX_NATIVE_PHP")")
scripts/native/test-w08-linux-runtime.sh \
  --candidate "$LINUX_NATIVE_PHP" \
  --fpm "$LINUX_NATIVE_SAPI/fpm/php-fpm"
```

### 3. Build the pinned reference PHP

Use a separate checkout or detached worktree at the pinned php-src baseline.
Do not alter the candidate checkout to build the reference.

For Debug NTS:

```bash
LINUX_REFERENCE_ROOT=/var/tmp/php-native-reference-debug-nts
git worktree add --detach \
  "$LINUX_REFERENCE_ROOT" \
  47355da494ba696b1bdb6d10448a225e742bd316
(
  cd "$LINUX_REFERENCE_ROOT"
  ./buildconf --force
  ./configure \
    --disable-all \
    --enable-cli \
    --disable-cgi \
    --disable-phpdbg \
    --enable-pcntl \
    --enable-zend-test \
    --enable-debug \
    --disable-zts \
    --without-pear
  make -j1 sapi/cli/php
)
LINUX_REFERENCE_PHP="$LINUX_REFERENCE_ROOT/sapi/cli/php"
test -x "$LINUX_REFERENCE_PHP"
```

Match `--enable-zts`, `--enable-address-sanitizer`,
`--enable-undefined-sanitizer`, and debug/release settings to each candidate
profile exactly, as done in `.github/workflows/native-engine.yml`.

### 4. Complete Debug NTS php-src suite

Run from the candidate source root, not from an out-of-tree build directory:

```bash
TEST_PHP_SRCDIR="$PWD" \
TEST_PHP_EXECUTABLE="$LINUX_NATIVE_PHP" \
  "$LINUX_NATIVE_PHP" -n run-tests.php \
    -n -q -j1 --set-timeout 600 \
    -W /var/tmp/linux-native-debug-nts-results.tsv
```

Run the same corpus with the matching reference binary and a separate `-W`
file. Compare it with the existing inline comparator in the Linux job of
`.github/workflows/native-engine.yml`. Do not add a new comparator script to
the repository. Acceptance requires:

- identical candidate/reference corpus membership;
- no incomplete candidate run;
- no candidate failures;
- no additional candidate skips.

Record every remaining failure with its PHPT path, candidate/reference status,
exit code, and whether it reproduces in isolation. Do not classify a failure
as pre-existing without current reference evidence.

### 5. Debug ZTS, Release, ASan, and UBSan

Only after Debug NTS is green:

```bash
scripts/native/build.sh \
  --profile linux-amd64-native-debug-zts --jobs 1
scripts/native/test-smoke.sh \
  --profile linux-amd64-native-debug-zts --jobs 1

scripts/native/build.sh \
  --profile linux-amd64-native-release-nts --jobs 1
scripts/native/test-smoke.sh \
  --profile linux-amd64-native-release-nts --jobs 1

scripts/native/test-sanitizers.sh \
  --profile linux-amd64-native-asan-nts --jobs 1
scripts/native/test-sanitizers.sh \
  --profile linux-amd64-native-ubsan-nts --jobs 1
```

The sanitizer wrapper supplies the repository defaults. On Linux they include
ASan leak detection and fail on diagnostics. Do not install suppressions or
turn leak detection off. Run the focused native/OPcache PHPT set and then the
complete candidate/reference corpus for each matching profile, not only the
three-test sanitizer smoke.

## LinuxX64 parity questions to resolve

After the file compiles, audit these known target differences with focused
PHPTs and disassembly rather than copying AArch64 code mechanically:

1. `cur_func_may_emit_calls` remains plan-wide in LinuxX64; determine the
   correct function-specific call/unwind facts.
2. Verify existing fixed assignments for register-authoritative pointers and
   boxed PHIs against x86-64 register pressure, call clobbers, and benchmarks.
3. The common planner has a narrow AArch64-only by-reference lowering
   capability. Either implement the equivalent LinuxX64 capability with the
   same semantic PHPTs or explicitly reject it as a target capability; do not
   silently route through another executor.
4. Recheck direct local component calls, typed bodies, boxed temporary guards,
   reference cells, exception routing, and stack-limit paths under SysV x86-64
   ABI rules.
5. Any narrower Linux allocator rule must be justified by real AMD64 behavior
   and must not be generalized into the common adaptor merely to make x64
   compile.

Relevant current regression tests include:

```text
ext/native_mir_test/tests/w11p_inline_boxed_native_frames.phpt
ext/native_mir_test/tests/w11p_inline_typed_user_frames.phpt
ext/native_mir_test/tests/w11p_canonical_boxed_phis.phpt
ext/native_mir_test/tests/w14_effect_closed_scalar_inline.phpt
ext/native_mir_test/tests/w14_boxed_temporary_typed_call.phpt
Zend/tests/stack_limit/stack_limit_014.phpt
Zend/tests/pipe_operator/wrapped_chains.phpt
```

## Product build and persistent FPM acceptance

Build the existing product profile after all debug/sanitizer correctness gates
are green:

```bash
LINUX_PRODUCT_PHP=$(
  scripts/native/build.sh \
    --profile linux-amd64-native-product-release-nts \
    --jobs 1 \
    --print-binary | tail -n 1
)
LINUX_PRODUCT_SAPI=$(dirname "$(dirname "$LINUX_PRODUCT_PHP")")
LINUX_PRODUCT_FPM="$LINUX_PRODUCT_SAPI/fpm/php-fpm"
LINUX_PRODUCT_CGI="$LINUX_PRODUCT_SAPI/cgi/php-cgi"
LINUX_PRODUCT_PHPDBG="$LINUX_PRODUCT_SAPI/phpdbg/phpdbg"
test -x "$LINUX_PRODUCT_PHP"
test -x "$LINUX_PRODUCT_FPM"
test -x "$LINUX_PRODUCT_CGI"
test -x "$LINUX_PRODUCT_PHPDBG"
```

Run the product-native PHPTs:

```bash
TEST_PHP_SRCDIR="$PWD" \
TEST_PHP_EXECUTABLE="$LINUX_PRODUCT_PHP" \
TEST_PHPDBG_EXECUTABLE="$LINUX_PRODUCT_PHPDBG" \
  "$LINUX_PRODUCT_PHP" -n run-tests.php \
    -n -q -j1 --show-diff --set-timeout 600 \
    -p "$LINUX_PRODUCT_PHP" \
    ext/opcache/tests/native_*.phpt \
    sapi/phpdbg/tests/native_engine_basic.phpt
```

Run the real persistent, preloaded, forked FPM worker path:

```bash
scripts/native/test-native-product-fpm.sh \
  --candidate "$LINUX_PRODUCT_PHP" \
  --fpm "$LINUX_PRODUCT_FPM"
```

It must finish with the existing PASS record covering preload, the same worker
across repeated requests, OPcache hits and resets, include generations,
include/eval, and suspended Fiber state. This is required product evidence; a
CLI-only PHPT pass is not a substitute.

Then run the complete product candidate/reference corpus with matching CLI,
CGI, phpdbg, and FPM-capable builds exactly as the Linux Release job does.

## Existing performance gates

Use only `scripts/native/benchmark-native-performance.py`; do not add another
benchmark or metrics path. Reproduce all three Linux Release gates from
`.github/workflows/native-engine.yml`:

```text
diagnostic, target linux-amd64-prod, suite all
product-cli with OPcache on, target linux-amd64-prod, suite all
product-fpm with OPcache on, target linux-amd64-prod, suite all
```

The baseline commits are listed above. All commands must use `--enforce`.
Report runtime, RSS, and cold-compile threshold failures exactly. Do not remove
an existing product gate because an older baseline crashes or returns different
semantic output; distinguish baseline incompatibility from candidate failure.

## Git workflow and return handover

Keep Linux changes narrowly scoped. A likely first commit, if the alias repair
is sufficient and independently validated, is:

```text
fix(native): disambiguate x86-64 compiler types
```

Never mention assistants or assisted development in commit messages. Before
each commit:

```bash
git diff --check
git status --short
git diff --cached --name-only
git diff --cached --check
```

Stage only intended source and PHPT files. Never stage generated PHPT output,
logs, build products, `bla.md`, or `fixes.md`. Do not stage later handover edits
unless explicitly requested. Do not modify unrelated Darwin code merely to make
a Linux commit look symmetric.

Before pushing, incorporate the latest `origin/master` without discarding
parallel Darwin work. Use conventional commits under 72 characters. Push only
after the scoped local milestone is green.

The Linux return report must contain:

- exact resulting commit(s) and pushed branch/head;
- every build/test command with exit status;
- Debug NTS/ZTS, Release, ASan, and UBSan results separately;
- focused native/OPcache counts;
- full candidate/reference corpus counts and exact differences;
- Product CLI/CGI/phpdbg and persistent FPM results;
- all three performance-gate summaries;
- `git diff --check` and final `git status --short`;
- every unresolved failure or unavailable check, without an optimistic parity
  claim.

Linux/AMD64 is complete only when the backend builds, the existing semantic
and product paths are green, the full php-src comparison has no candidate
regressions or extra skips, persistent Product FPM is proven, and the existing
performance gates pass. Until then, Darwin success is not full PHP-on-TPDE
parity.
