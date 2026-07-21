# Native W00 reproducible build harness

This harness provides repeatable commands and isolated build trees for the six
mandatory W00 profiles. It records the source state, configure command,
toolchain, flags, runtime properties, and binary digest. It does **not** claim
that two binaries are bit-identical.

## Requirements

The primary target is Linux/ELF x86-64. Required preinstalled tools are Bash,
Git, SHA-256 utilities, Python 3.9 or newer, Autoconf/Autoheader, Bison, Re2c,
Make, pkg-config, flock, and a C compiler. The minimal `--disable-all` build uses
bundled/core dependencies and builds the CLI plus static OPcache. No script
downloads packages or installs dependencies.

AArch64 uses the same profiles and scripts. An Ubuntu AArch64 GitHub-hosted or
self-hosted runner can set `NATIVE_WORK_ROOT` and run the commands below.

## Profiles

| Profile | Debug | Thread safety | Sanitizer |
|---|---:|---:|---|
| `debug-nts` | yes | NTS | none |
| `release-nts` | no | NTS | none |
| `debug-zts` | yes | ZTS | none |
| `release-zts` | no | ZTS | none |
| `asan-nts` | yes | NTS | AddressSanitizer + leak detection |
| `ubsan-nts` | yes | NTS | UndefinedBehaviorSanitizer, no recovery |

Profile definitions live in `profiles/*.env`. They use configure switches
verified at the pinned php-src commit. OPcache has no enable switch there: it is
an unconditional static extension, and each successful build checks that it is
loaded.

## Usage

```bash
scripts/native/configure-dev.sh
scripts/native/build.sh --profile debug-nts --jobs 8
scripts/native/build.sh --profile release-zts --print-binary
scripts/native/test-smoke.sh --profile release-nts
scripts/native/test-smoke.sh --php-binary /absolute/path/to/php
scripts/native/test-sanitizers.sh
```

Every public script supports `--help`. An unknown profile, missing tool, build
failure, PHPT failure, or sanitizer diagnostic returns non-zero.

`NATIVE_JOBS` sets the default parallelism. `CC`, `CFLAGS`, `CPPFLAGS`, and
`LDFLAGS` are honored and included in the configuration fingerprint and build
manifest. `NATIVE_BASE_COMMIT` may explicitly override the worktree-ID input;
by default the W00 base `47355da494ba696b1bdb6d10448a225e742bd316` is used,
including in shallow CI checkouts.

## Isolation and artifacts

The default work root is `${TMPDIR:-/tmp}/php-native-w00`. Set an absolute
external location when persistent artifacts are wanted:

```bash
export NATIVE_WORK_ROOT=/var/tmp/php-native-w00
```

The layout is:

```text
${NATIVE_WORK_ROOT}/<worktree-name>-<path+base-commit-hash>/<profile>/
  build/                         out-of-tree build
  logs/configure.log
  logs/build.log
  artifacts/smoke/*.log
  artifacts/smoke/phpt-results.tsv
  artifacts/smoke/smoke-summary.json
  configure-state.json
  build-manifest.json
```

The ID hashes the canonical worktree path and base commit, so distinct
worktrees cannot share builds or caches even for the same profile. Profiles
also have disjoint directories. `buildconf` writes only Git-ignored generated
files in the assigned source worktree and is serialized per worktree; all
compiled objects, logs, manifests, and test artifacts remain external.

Configure is reused only when its fingerprint matches the repository commit,
tracked/untracked source state, profile, configure arguments, compiler version,
and relevant flag environment. A mismatch resets only that validated external
profile build directory and reconfigures it. Re-running a compatible build is
idempotent and lets Make rebuild only what changed.

## Smoke and sanitizer behavior

The focused PHPT list contains one Zend test, one CLI test, and one OPcache CLI
test. The harness also runs `php -v` and `php -m`, retains raw output, writes a
parseable JSON summary, and propagates failures.

ASan defaults to:

```text
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1:strict_string_checks=1
LSAN_OPTIONS=exitcode=23:print_suppressions=1
USE_ZEND_ALLOC=0
```

UBSan defaults to:

```text
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

Caller-provided values are preserved. No sanitizer suppressions are installed,
and diagnostic signatures in any smoke log fail the run even if a subprocess
returns zero.

## CI

`.github/workflows/native-engine.yml` runs the contract tests and the actively
used debug NTS, debug ZTS, ASan, and UBSan profiles on Linux x86-64. It reuses
`.github/actions/apt-x64` instead of maintaining a second package list and
requires no secrets. Release profiles remain available for explicit local or
release qualification runs.

## Troubleshooting

- A missing prerequisite names the exact tool. Install it outside the harness
  or use the focused CI runner, which reuses php-src's Linux dependency action.
- Inspect `logs/configure.log` or `logs/build.log` after compilation failures.
- Inspect `artifacts/smoke/phpt.log` and `smoke-summary.json` after test failures.
- Use `--force-configure` on `build.sh` only when intentionally discarding one
  profile build tree; ordinary source/profile changes invalidate it automatically.
- Keep `NATIVE_WORK_ROOT` outside the repository. No `.gitignore` change is
  needed for the default layout.
