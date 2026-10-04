# ADR 0006: Target platforms

## Status

Accepted.

## Context

Native code generation depends on object format, executable-memory policy,
calling conventions, relocations, unwind behavior, and instruction-set details.
The engine supports Linux/ELF x86-64 and Darwin arm64.

## Decision

The native engine has two targets: Linux/ELF x86-64 (`Zend/Native/TPDE/LinuxX64`,
`Zend/Native/Runtime/LinuxX64`) and Darwin arm64 (`Zend/Native/TPDE/DarwinA64`,
`Zend/Native/Runtime/DarwinA64`). ZNMIR, lowering, the plan and the runtime are
shared and architecture-independent.

Each host builds only its own backend: configure selects the target from the
host and rejects every other host, and the backend dispatches to the host
target at compile time. No binary contains code for the other target.

Shared-code changes keep both backends correct. On Linux,
`scripts/native/check-darwin-backend.sh` syntax-checks the Darwin backend with
the profile's flags; the test tiers run it after the debug build. The Darwin
CI job builds and tests the Darwin backend.

## Consequences

- A target's emitter and publisher compile only on its own host; the other
  backend is checked, not linked.
- Removing or disabling a target requires a new ADR.

## Verification impact

Linux validation runs on a Linux/ELF x86-64 runner and includes the Darwin
syntax check; Darwin validation runs on the macOS arm64 CI runner.
Unsupported hosts fail at configure.
