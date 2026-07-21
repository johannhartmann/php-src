# ADR 0006: Linux/ELF x86-64 first, AArch64 second

## Status

Accepted on 2026-07-17.

## Context

Native code generation depends on object format, executable-memory policy,
calling conventions, relocations, unwind behavior, and instruction-set details.
Claiming broad support before each combination has a runner and evidence would
turn untested code paths into an implicit compatibility promise.

## Decision

Use Linux/ELF x86-64 as the primary supported platform. Treat AArch64 as the
documented second target once an explicit runner, ABI contract, backend
verification, and differential suite exist. Reject all other target
combinations explicitly until an ADR and the target tests accept them.

Keep ZNMIR architecture-independent and isolate target mechanics under
`Zend/Native/TPDE/`.

## Consequences

- W00 infrastructure may document AArch64 requirements but must not claim that
  the native engine already supports AArch64.
- Cross-target semantics share one MIR and differential oracle while backend
  evidence remains target-specific.
- New operating systems, object formats, and architectures require explicit
  enablement rather than accidental compilation.

## Alternatives

- Enabling every TPDE upstream target was rejected because upstream code does
  not establish PHP ABI or semantic compatibility.
- Designing only for x86-64 was rejected because a named second target exposes
  architecture leakage early.

## Verification impact

Primary-platform validation requires a Linux/ELF x86-64 runner. AArch64 is
supported only with the same semantic, ABI, relocation, unwind, and concurrency
coverage. Unsupported combinations must fail clearly.
