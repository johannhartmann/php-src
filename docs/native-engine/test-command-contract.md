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

## Commands

### `scripts/native/configure-dev.sh`

Create an isolated native-engine development configuration without modifying a
different profile's build tree. Exit `0` only after configuration completes and
the requested profile is identifiable for `build.sh`.

### `scripts/native/build.sh`

Build the selected profile. Configure is reused while its build-system inputs,
profile, toolchain and flags are unchanged and rerun when they change; source
edits rebuild incrementally. Exit `0` only when all requested build targets
complete.

### `scripts/native/test-smoke.sh`

Run the bounded smoke suite against an explicitly selected PHP binary. Exit `0`
only when every required smoke check passes; skips caused by missing required
capabilities are failures.

### `scripts/native/test-sanitizers.sh`

Build or select the declared sanitizer profile and run its required tests. Exit
`0` only when tests pass and no required sanitizer diagnostic is present.

### `scripts/native/test-phpt.sh`

Run the PHPT tiers: `--tier quick` (native PHPTs plus given paths), `--tier
commit` (full debug suites) and `--tier full` (commit, the commit suites under
deoptimization stress, ASan and UBSan). `--deopt-stress PERIOD` runs the debug
suites with every PERIOD-th deoptimization point transferring its frame to a
new activation.
Exit `0` only when no test fails, or, with `--baseline`, when no test fails
that the baseline does not list.

### `scripts/native/check-darwin-backend.sh`

On a Linux host, syntax-check the Darwin arm64 backend with the configured
profile's compile flags. Exit `0` when every Darwin source parses, `1` on a
compile error, `3` when the profile is not configured.

## Changes

Change commands, options and exit classes freely together with their callers
(scripts, CI) in the same commit.
