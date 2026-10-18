# Native test-command contract

## Common behavior

Every command must:

- run non-interactively by default and resolve repository-relative paths from
  the repository root rather than the caller's current directory;
- print diagnostics to standard error and reserve standard output for declared
  machine-readable or primary result output;
- fail if a required stage, input, tool, or result is missing;
- preserve the underlying failure and never turn a failed test into success;
- terminate all child processes before returning.

The stable exit classes are:

| Exit | Meaning |
|---:|---|
| `0` | Every requested stage completed and satisfied its contract. |
| `2` | Command-line usage or an invalid caller-supplied value. |
| `3` | A declared prerequisite or required input is unavailable. |
| other nonzero | Configuration, build, test, sanitizer, validation, or internal failure. |

Callers must treat every nonzero status as failure. Implementations may preserve
a meaningful nonzero child status when it does not collide with `2` or `3`.

Profiles are the `scripts/native/profiles/*.env` files. Build trees, logs and
manifests live below `NATIVE_WORK_ROOT` (an absolute path, default
`${TMPDIR:-/tmp}/php-native`), one directory per worktree and profile.
`NATIVE_JOBS` sets the default job count, which is otherwise the CPU count.
The default profile `debug-nts` is stock PHP without the native engine;
native work names a `*-native-*` profile.

## Commands

### `scripts/native/configure-dev.sh`

`configure-dev.sh [--profile PROFILE] [--force] [--print-build-dir]`

This wrapper passes its arguments to `configure.sh`. It creates an isolated
configuration for one profile without modifying any other profile's build
tree. Configure is reused while its fingerprint is unchanged. The fingerprint
covers the build-system inputs (`configure.ac`, `buildconf`, `build/*`,
`*.m4`, `config*.m4`, `Makefile.frag*`), the profile file, the compiler,
`CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS` and the configure arguments. `--force`
reruns configure regardless. `--print-build-dir` prints the build directory
instead of `BUILD_DIR=<dir>`. Exit `0` only after configuration completes
and the profile is identifiable for `build.sh`.

### `scripts/native/build.sh`

`build.sh [--profile PROFILE] [--jobs N] [--force-configure] [--print-binary]`

Configures the profile through `configure.sh` (`--force-configure` passes
`--force`), then builds it with `make -jN`. Source edits rebuild
incrementally. A change of the profile's extra compile flags, function order
or PGO profile rebuilds every object once. The command checks that the binary
exists, that `PHP_DEBUG`, `PHP_ZTS` and the built-in OPcache match the
profile, and that sanitizer flags are present. It then writes
`build-manifest.json`. It prints `MANIFEST=<path>` and then `BINARY=<path>`,
or with `--print-binary` the bare binary path as the final line. Exit `0`
only when all of this completes.

### `scripts/native/test-smoke.sh`

`test-smoke.sh [--profile PROFILE | --php-binary PATH] [--jobs N]`

Runs the smoke suite against a profile, which it builds first, or against an
explicit PHP binary. The suite consists of `php -n -v`, `php -n -m` (which
must list Zend OPcache) and three PHPTs:

- `Zend/tests/add_001.phpt`;
- `sapi/cli/tests/001.phpt`;
- `ext/opcache/tests/001_cli.phpt`.

`NATIVE_PHPT_RUNNER` names a separate plain PHP that runs `run-tests.php`
(with `-n`, without `PHP_INI_SCAN_DIR` and `PHPRC`). Otherwise the binary
under test runs it. Logs and `smoke-summary.json` go below
`NATIVE_WORK_ROOT`. Exit `0` only when every required smoke check passes and
no sanitizer diagnostic appears; skips caused by missing required
capabilities are failures. A failed check exits `1`.

### `scripts/native/test-sanitizers.sh`

`test-sanitizers.sh [--profile PROFILE] [--jobs N]`

Runs `test-smoke.sh` for the given address- or undefined-sanitizer profile.
Without `--profile` it runs the `asan-nts` and `ubsan-nts` profiles. Strict
`ASAN_OPTIONS`/`UBSAN_OPTIONS` defaults apply, with leak detection where the
platform supports it and no suppressions. Exit `0` only when every profile
passes and no required sanitizer diagnostic is present; a failed profile
exits `1`.

### `scripts/native/test-phpt.sh`

```
test-phpt.sh [--tier quick|commit|full] [--jobs N] [--no-build]
             [--show-slow MS] [--baseline FILE] [--deopt-stress PERIOD]
             [PATH...]
```

Runs the PHPT tiers on the host's `*-native-*` profiles. Each profile is
built incrementally first, unless `--no-build` is given.

- `quick` (default) runs `ext/native_mir_test/tests` plus `PATH`s on the
  debug profile.
- `commit` runs the following plus `PATH`s on the debug profile:
  - `Zend/tests`, `ext/native_mir_test/tests` and `ext/opcache/tests`;
  - `ext/standard/tests/{array,math,strings}`;
  - `ext/spl/tests` and `ext/reflection/tests`.
- `full` runs, in order:
  - the commit suites;
  - the commit suites again under deoptimization stress (period 1) and
    layout stress (`ZEND_NATIVE_LAYOUT_PROFILE='*'`);
  - the ASan and UBSan profiles. They are built concurrently and run one
    after the other on `Zend/tests`, `ext/native_mir_test/tests`,
    `ext/opcache/tests` and `ext/standard/tests/{array,math}` plus `PATH`s.

On Linux every tier also runs `check-darwin-backend.sh` against the debug
profile.

Options:

- `--deopt-stress PERIOD` runs the debug suites with
  `ZEND_NATIVE_DEOPT_STRESS=PERIOD` and layout stress instead of the plain
  run.
- `--show-slow MS` reports tests slower than `MS` milliseconds (default
  3000).
- `--baseline FILE` also lists the failures that `FILE`, one test path per
  line, does not list.

`NATIVE_PHPT_RUNNER` must name a plain PHP CLI that runs `run-tests.php`;
without it the command exits `3`.

Each run writes `phpt-<tier>[-deopt].log`, `-failures.txt` and `-slow.txt`
to the profile's log directory. Exit codes:

- `0` when no test fails, or, with `--baseline`, when no test fails that
  the baseline does not list;
- `1` on test failures;
- `2` on usage errors;
- `3` on missing prerequisites (no runner, or no native profiles for the
  host).

### `scripts/native/check-darwin-backend.sh`

`check-darwin-backend.sh [--profile PROFILE]`

On a Linux host, syntax-checks the Darwin arm64 back-end sources with the
compile flags of the configured profile (default
`linux-amd64-native-debug-nts`). Exit codes:

- `0` when every Darwin source parses;
- `1` on a compile error;
- `2` on usage errors;
- `3` when the profile is not configured or its compile command cannot be
  derived.

## Changes

Change commands, options and exit classes freely, together with their callers
(other scripts and documentation) in the same commit.
