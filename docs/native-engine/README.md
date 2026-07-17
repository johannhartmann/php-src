# Native engine contracts

This directory records the architecture and working contracts for the native
engine full-cutover project. W00 establishes rules, build/test command names,
and gates only; it does not add a compiler or change PHP runtime behavior.

Repository-wide and path-specific agent rules live in:

- [repository instructions](../../AGENTS.md)
- [Zend native engine](../../Zend/Native/AGENTS.md)
- [canonical MIR](../../Zend/Native/MIR/AGENTS.md)
- [TPDE backend](../../Zend/Native/TPDE/AGENTS.md)
- [native runtime](../../Zend/Native/Runtime/AGENTS.md)
- [OPcache](../../ext/opcache/AGENTS.md)
- [native tests](../../tests/native/AGENTS.md)

## Architecture decisions

1. [Atomic full cutover without production VM fallback](adr/0001-atomic-full-cutover.md)
2. [One canonical ZNMIR](adr/0002-canonical-znmir.md)
3. [Native baseline tier and resume targets](adr/0003-native-baseline-and-resume.md)
4. [Zend-compatible baseline frames](adr/0004-zend-compatible-baseline-frame.md)
5. [Immutable code versions and entry cells](adr/0005-immutable-code-and-entry-cells.md)
6. [Primary and secondary target platforms](adr/0006-target-platforms.md)
7. [Relocatable OPcache persistence](adr/0007-opcache-persistence.md)
8. [Differential testing as the semantic oracle](adr/0008-differential-oracle.md)

## Delivery contracts

- [Codex task template](codex-task-template.md)
- [Codex review template](codex-review-template.md)
- [Pull-request conventions](pr-conventions.md)
- [Native test-command contract](test-command-contract.md)

The generated wave status subtree, `docs/native-engine/waves/`, is owned by the
wave-gate work and is intentionally outside W00-A.
