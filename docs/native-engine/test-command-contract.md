# Native test-command contract

## Status and ownership

W00 fixes the public command names and exit semantics below before the scripts
exist. W00-A defines this contract but does not implement any script.

- W00-B owns `configure-dev.sh`, `build.sh`, `test-smoke.sh`, and
  `test-sanitizers.sh` under `scripts/native/`.
- W00-D owns `scripts/native/wave-gate.py` and
  `docs/native-engine/waves/**`.

Until those work streams are integrated, a missing command is an expected W00
delivery gap, not a successful check and not permission to add a differently
named wrapper.

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

Build the explicitly selected configured profile. Do not configure implicitly
or reuse an incompatible profile. Exit `0` only when all requested build targets
complete.

### `scripts/native/test-smoke.sh`

Run the bounded smoke suite against an explicitly selected PHP binary. Exit `0`
only when every required smoke check passes; skips caused by missing required
capabilities are failures.

### `scripts/native/test-sanitizers.sh`

Build or select the declared sanitizer profile and run its required tests. Exit
`0` only when tests pass and no required sanitizer diagnostic is present.

### `scripts/native/wave-gate.py`

Validate and record declared wave-result JSON, then generate the wave status
view owned by W00-D. Reject missing, malformed, incompatible, or unsuccessful
required results. Exit `0` only when validation and the requested gate operation
complete atomically.

## Compatibility

Keep these paths and exit classes stable across later waves. Additive options
must preserve non-interactive behavior. Any incompatible CLI, result-format, or
exit-semantics change requires an ADR, coordinated consumer updates, and fixture
coverage before integration.
