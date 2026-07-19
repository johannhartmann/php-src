# Native engine contracts

This directory records the architecture and working contracts for the native
engine full-cutover project. W00 establishes rules, build/test command names,
and gates; W01 freezes semantic contracts. Neither wave adds a compiler or
changes PHP runtime behavior.

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
9. [Native frame state and safepoints](adr/0009-native-frame-state-and-safepoints.md)
10. [Bailout, exception, suspend, and resume ABI](adr/0010-bailout-exception-suspend-resume-abi.md)
11. [Canonical ZNMIR core contract](adr/0011-canonical-znmir-core-contract.md)
12. [ZNMIR text, diagnostics, and verification](adr/0012-znmir-text-diagnostics-verification.md)
13. [Proof-closed scalar lowering](adr/0013-w03-proof-closed-scalar-profile.md)
14. [Deterministic lowering boundary](adr/0014-lowering-source-view-and-registry.md)
15. [W04 control-flow source contract](adr/0015-w04-control-flow-source-contract.md)
16. [Reducible CFGs and edge statepoints](adr/0016-w04-reducible-cfg-and-edge-statepoints.md)
17. [W05 atomic call-sequence model](adr/0019-w05-call-sequence-model.md)
18. [Capability receipts instead of stage numbers](adr/0020-capability-receipts-not-stage-numbers.md)

## Delivery contracts

- [Codex task template](codex-task-template.md)
- [Codex review template](codex-review-template.md)
- [Pull-request conventions](pr-conventions.md)
- [Native test-command contract](test-command-contract.md)
- [Native frame semantics](semantics/frames/README.md)
- [W03 lowering profile](lowering/README.md)
- [W04 control-flow contracts](control-flow/contracts/source-cfg.md)
- [W05 direct-user-call contracts](calls/contracts/call-sequence.md)

The generated wave status subtree, `docs/native-engine/waves/`, is owned by the
wave-gate work and is intentionally outside W00-A.
