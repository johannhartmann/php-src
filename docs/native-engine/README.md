# Native engine

The native engine replaces the Zend VM with native code generated through TPDE
(Linux x86-64). This directory holds its architecture decisions and the
semantic contracts the implementation follows. Agent rules live in the
`AGENTS.md` files of the repository root and of `Zend/Native/**`.

## Architecture decisions

- [0001 No production VM fallback](adr/0001-atomic-full-cutover.md)
- [0002 One canonical ZNMIR](adr/0002-canonical-znmir.md)
- [0003 Generic native code and resume targets](adr/0003-generic-native-code-and-resume.md)
- [0005 Immutable code versions and entry cells](adr/0005-immutable-code-and-entry-cells.md)
- [0006 Target platforms](adr/0006-target-platforms.md)
- [0007 Relocatable OPcache persistence](adr/0007-opcache-persistence.md)
- [0008 Differential testing against stock PHP](adr/0008-differential-oracle.md)
- [0010 Bailout, exception, suspend and resume ABI](adr/0010-bailout-exception-suspend-resume-abi.md)
- [0011 ZNMIR core contract](adr/0011-canonical-znmir-core-contract.md)
- [0012 ZNMIR text, diagnostics and verification](adr/0012-znmir-text-diagnostics-verification.md)
- [0024 Typed lowering](adr/0024-typed-lowering-tier.md)
- [0025 Native state is canonical](adr/0025-native-canonical-execution.md)

ADR 0025 defines the execution model: native state is canonical, Zend frames
are built on demand where PHP state is observable, compiled functions call each
other through a native convention, and speculation deoptimizes into the generic
native version of a function.

## Contracts

- [Frame semantics](semantics/frames/README.md): frame layout, safepoints,
  bailout, exceptions, suspension and resume.
- [Effects and ownership](semantics/effects/README.md): effect, memory-domain
  and ownership model of the MIR.
- [MIR](mir/README.md) and its [text format](mir/text-format.md).
- [Build and test commands](test-command-contract.md).
