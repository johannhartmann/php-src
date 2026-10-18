# ADR 0006: Target platforms

## Status

Accepted.

## Context

Native code generation depends on object format, executable-memory policy,
calling conventions, relocations, unwind behavior, and instruction-set details.

## Decision

The native engine has two targets: Linux/ELF x86-64
(`Zend/Native/TPDE/LinuxX64`, `Zend/Native/Runtime/LinuxX64`) and Darwin arm64
(`Zend/Native/TPDE/DarwinA64`, `Zend/Native/Runtime/DarwinA64`). ZNMIR, lowering, the plan and the runtime
helpers are shared and architecture-independent; each target has its own code
generator and publisher.

Each host builds only its own backend: `ext/native_mir_test/config.m4`
selects the target from the host and rejects every other host, and the
backend compiles for `ZEND_NATIVE_HOST_TARGET`
(`Zend/Native/TPDE/Common/zend_tpde_backend.h`). No binary contains code for
the other target.

Shared-code changes keep both backends correct. On Linux,
`scripts/native/check-darwin-backend.sh` syntax-checks the Darwin backend with
the profile's flags; `scripts/native/test-phpt.sh` runs it after the debug
build of every tier. Darwin itself is built and tested on a macOS arm64 host.

## Consequences

- A target's emitter and publisher compile only on its own host; the other
  backend is checked, not linked.
- Typed lowering (ADR 0024), speculation, entry variants and layout training
  are implemented for Linux x86-64 only; Darwin arm64 lowers every function
  with the untyped `zend_mir_lower_zend_op_array()`.
- Removing or disabling a target requires changing this ADR.

## Verification impact

Linux validation runs on a Linux/ELF x86-64 host and includes the Darwin
syntax check; Darwin validation runs on a macOS arm64 host.
Unsupported hosts fail at configure.
