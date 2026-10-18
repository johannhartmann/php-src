# Native build and test harness

These scripts configure, build and test the profiles in `profiles/*.env` in
isolated build trees outside the worktree. Each build records the source
state, configure command, toolchain, flags, runtime properties and binary
digest. It does **not** claim that two binaries are bit-identical.

[`TPDE.md`](../../TPDE.md) explains the engine, the profiles, the
environment variables and the test tiers; this file covers the harness
itself. Exit codes follow
[`docs/native-engine/test-command-contract.md`](../../docs/native-engine/test-command-contract.md).

## Requirements

Required preinstalled tools are Bash, Git, SHA-256 utilities, Python 3.9 or
newer, Autoconf/Autoheader, Bison, Re2c, Make, pkg-config and a C compiler;
the native profiles need clang/clang++ (`PROFILE_CC`). No script downloads
packages or installs dependencies.

Native profiles exist for Linux x86-64 (`linux-amd64-native-*`) and Darwin
arm64 (`darwin-arm64-native-*`). The plain `debug-nts`, `debug-zts`,
`release-nts`, `release-zts`, `asan-nts` and `ubsan-nts` profiles build
stock php-src.

## Profiles

Every profile builds with `--disable-all --enable-cli` plus its own
configure flags (`PROFILE_CONFIGURE_FLAGS`); native profiles also enable
`pcntl`. The `linux-amd64-native-*` profiles build with clang (`PROFILE_CC`)
and set `PROFILE_EXTRA_CFLAGS`: `-Og` for `debug-nts` and `debug-zts`, `-O1`
for the sanitizer profiles and `-fomit-frame-pointer` for the release
profiles. The three Linux release profiles (`release-nts`,
`product-release-nts`, `app-release-nts`) also link with the function order
(`PROFILE_FUNCTION_ORDER`) and compile with the clang PGO profile
(`PROFILE_PGO_PROFILE`) in `layout/`; see
[`layout/README.md`](layout/README.md).

## Usage

```bash
scripts/native/configure-dev.sh --profile linux-amd64-native-debug-nts
scripts/native/build.sh --profile linux-amd64-native-debug-nts --print-binary
NATIVE_PHPT_RUNNER=/path/to/plain/php scripts/native/test-phpt.sh --tier quick
scripts/native/test-smoke.sh --profile linux-amd64-native-debug-nts
scripts/native/test-sanitizers.sh --profile linux-amd64-native-asan-nts
scripts/native/check-darwin-backend.sh
```

The default profile of `configure-dev.sh`, `configure.sh` and `build.sh` is
`debug-nts`, which is stock PHP; always pass a native profile.
`test-sanitizers.sh` without `--profile` runs `asan-nts` and `ubsan-nts`.

A host builds only its own target backend. On Linux,
`check-darwin-backend.sh` syntax-checks the Darwin arm64 backend with the
configured profile's compile flags (default `linux-amd64-native-debug-nts`);
`test-phpt.sh` runs it after the debug build.

Every public script supports `--help`. An unknown profile, missing tool,
build failure, PHPT failure, or sanitizer diagnostic returns non-zero.

`NATIVE_JOBS` sets the default parallelism (otherwise the CPU count). `CC`,
`CFLAGS`, `CPPFLAGS`, and `LDFLAGS` are honored and included in the
configuration fingerprint and build manifest. `NATIVE_BASE_COMMIT` may
override the base commit of the worktree ID; by default it is the baseline
`47355da494ba696b1bdb6d10448a225e742bd316`.

## Isolation and artifacts

The default work root is `${TMPDIR:-/tmp}/php-native`. Set an absolute
external location when persistent artifacts are wanted:

```bash
export NATIVE_WORK_ROOT=/var/tmp/php-native
```

The layout is:

```text
${NATIVE_WORK_ROOT}/<worktree-name>-<path+base-commit-hash>/<profile>/
  build/                         out-of-tree build
  logs/configure.log
  logs/build.log
  logs/phpt-<tier>*.log          test-phpt.sh logs, failures and slow lists
  artifacts/smoke/*.log
  artifacts/smoke/phpt-results.tsv
  artifacts/smoke/smoke-summary.json
  configure-state.json
  build-manifest.json
```

The ID hashes the canonical worktree path and base commit, so distinct
worktrees cannot share builds or caches even for the same profile. Profiles
also have disjoint directories. `buildconf` writes only Git-ignored generated
files in the source worktree and is serialized per worktree; all compiled
objects, logs, manifests, and test artifacts remain external.

Configure is reused only when its fingerprint matches the build-system inputs
(`configure.ac`, `buildconf`, `build/`, `*.m4`, `config*.m4` and
`Makefile.frag*`), profile, configure arguments, compiler version, and
`CC`/`CFLAGS`/`CPPFLAGS`/`LDFLAGS`. Other source edits do not reconfigure:
Make rebuilds only what changed. A mismatch resets only that validated
external profile build directory and reconfigures it. A profile's
`PROFILE_EXTRA_CFLAGS`, function order and PGO flags are passed to Make as
`EXTRA_CFLAGS`/`EXTRA_CXXFLAGS` after configure's own flags; a change of
those flags cleans the objects once.

## Smoke and sanitizer behavior

`test-smoke.sh` runs `php -v`, `php -m` and three PHPTs
(`Zend/tests/add_001.phpt`, `sapi/cli/tests/001.phpt`,
`ext/opcache/tests/001_cli.phpt`), retains raw output, writes a parseable
JSON summary, and propagates failures. `test-sanitizers.sh` builds a
sanitizer profile and runs these smoke checks under it.

ASan defaults to:

```text
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1:strict_string_checks=1
LSAN_OPTIONS=exitcode=23:print_suppressions=1
USE_ZEND_ALLOC=0
```

On Darwin, `detect_leaks=0` and no `LSAN_OPTIONS` are set. UBSan defaults
to:

```text
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

Caller-provided values are preserved. No sanitizer suppressions are
installed, and diagnostic signatures in any smoke log fail the run even if a
subprocess returns zero.

## Troubleshooting

- A missing prerequisite names the exact tool. Install it outside the
  harness.
- Inspect `logs/configure.log` or `logs/build.log` after compilation
  failures.
- Inspect `artifacts/smoke/phpt.log` and `smoke-summary.json` after smoke
  failures, and `logs/phpt-<tier>.log` after `test-phpt.sh` failures.
- Use `--force-configure` on `build.sh` only when intentionally discarding
  one profile build tree; ordinary source/profile changes invalidate it
  automatically.
- Keep `NATIVE_WORK_ROOT` outside the repository. No `.gitignore` change is
  needed for the default layout.
