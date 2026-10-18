# Native engine

The native engine replaces the Zend VM with native code generated through TPDE
on Linux x86-64, with a reduced back-end on Darwin arm64. This directory holds
its architecture decisions and the semantic contracts the implementation
follows. [TPDE.md](../../TPDE.md) is the overview: what TPDE is, how the
engine integrates it, and how to build, run and test the engine. Agent rules
live in the `AGENTS.md` files of the repository root, `Zend/Native/**`,
`ext/opcache` and `tests/native`.

## Architecture decisions

- [0001 No production VM fallback](adr/0001-atomic-full-cutover.md): code
  is cut over atomically; a function that cannot be compiled is rejected,
  never interpreted.
- [0002 One canonical ZNMIR](adr/0002-canonical-znmir.md): every compiled
  function goes through one mid-level IR.
- [0003 Generic native code and resume targets](adr/0003-generic-native-code-and-resume.md):
  every function has a generic native version; resume targets enter it.
- [0005 Immutable code versions and entry cells](adr/0005-immutable-code-and-entry-cells.md):
  published code is never patched; entry cells publish new versions
  atomically.
- [0006 Target platforms](adr/0006-target-platforms.md): Linux x86-64 and
  Darwin arm64; each host builds only its own back-end.
- [0007 Relocatable OPcache persistence](adr/0007-opcache-persistence.md):
  pointer-free per-script bundles that each process imports and maps.
- [0008 Differential testing against stock PHP](adr/0008-differential-oracle.md):
  stock PHP is the semantic oracle, with no silent normalization.
- [0010 Bailout, exception, suspend and resume ABI](adr/0010-bailout-exception-suspend-resume-abi.md):
  how native code bails out, throws, suspends and resumes.
- [0011 ZNMIR core contract](adr/0011-canonical-znmir-core-contract.md): the
  versioned C contract of ZNMIR records, views and mutators.
- [0012 ZNMIR text, diagnostics and verification](adr/0012-znmir-text-diagnostics-verification.md):
  the text format, diagnostics and verifier of ZNMIR.
- [0024 Typed lowering](adr/0024-typed-lowering-tier.md): operations typed
  from Zend's type inference, with boxed value operations for the rest.
- [0025 Native state is canonical](adr/0025-native-canonical-execution.md):
  the execution model.

ADR 0025 defines the execution model:

- native state is canonical;
- Zend frames are built on demand where PHP state is observable;
- compiled functions call each other through a native convention;
- speculation deoptimizes into the generic native version of a function.

## Contracts

- [Shared vocabulary](semantics/vocabulary.md): identifiers shared by the
  effect model, MIR and lowering.
- [Frame semantics](semantics/frames/README.md): the index of the frame
  contracts and their validation.
  - [Frame contract](semantics/frames/frame-contract.md): physical frame and
    slot layout, ownership, roots and lifecycle.
  - [Safepoint contract](semantics/frames/safepoint-contract.md):
    canonicalization boundaries and the opline state at each one.
  - [Resume ABI](semantics/frames/resume-abi.md): exception, bailout,
    destructor reentry, suspension and native resume.
- [Effects and ownership](semantics/effects/README.md): the effect,
  memory-domain and ownership model of the MIR.
  - [Memory domains](semantics/effects/memory-domains.md): semantic memory
    domains and alias edges.
  - [Ownership and cleanup](semantics/effects/ownership-and-cleanup.md):
    ownership states and release obligations.
  - [Guard stability](semantics/effects/guard-stability.md): how long a
    guard fact stays valid.
  - [Code-motion examples](semantics/effects/code-motion-examples.md):
    allowed and forbidden reorderings, with their proofs.
- [MIR](mir/README.md): the ZNMIR core and its verification.
  - [Text format](mir/text-format.md): the canonical `znmir-text-v1`
    serialization.
- [Build and test commands](test-command-contract.md): the
  `scripts/native/` entry points, their options and exit codes.
