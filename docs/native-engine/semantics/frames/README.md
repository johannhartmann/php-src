# Native frame semantics

This directory freezes the W01 native baseline-frame, safepoint, bailout, and
resume contracts. It specifies observable behavior and metadata; it does not
add runtime code, public headers, or an optimizer ABI.

## Contract map

- [Baseline frame contract](baseline-frame-contract.md): physical frame and
  slot layout, ownership, roots, and lifecycle.
- [Safepoint contract](safepoint-contract.md): canonicalization boundaries and
  exact opline state before and after each boundary.
- [Resume ABI](resume-abi.md): exception, bailout, destructor reentry,
  generator/fiber suspension, and single-entry native resume.
- [Frame-state schema](frame-state.schema.json): required machine-readable
  fields for one canonical frame.
- [Frame-state examples](frame-state.examples.json): valid normal-call,
  exception, destructor, generator, and fiber states plus negative cases.
- [ADR 0009](../../adr/0009-native-frame-state-and-safepoints.md) and
  [ADR 0010](../../adr/0010-bailout-exception-suspend-resume-abi.md): accepted
  architecture decisions.

## Binding invariants

1. Observable execution uses Zend-compatible `zend_execute_data` and VM-stack
   slots. Native metadata supplements that ABI and cannot replace it.
2. Every safepoint has canonical opline, parent chain, roots, ownership, and
   cleanup state before control crosses the boundary.
3. Exceptions use `EG(exception)` plus explicit native control flow. Bailout is
   a nonlocal transfer.
4. Suspension owns persistent rooted state. Resume has one version-checked
   native entry addressed by resume ID.
5. Missing frame or resume metadata prevents code-version publication. There is
   no production VM fallback.

## Zend source anchors

These anchors identify the existing semantics this contract preserves:

| Subject | Source |
|---|---|
| `zend_execute_data` fields and call flags | [`zend_compile.h`](../../../../Zend/zend_compile.h#L645) |
| Frame-slot and argument address calculation | [`zend_compile.h`](../../../../Zend/zend_compile.h#L714) |
| VM-stack call allocation and cleanup | [`zend_execute.h`](../../../../Zend/zend_execute.h#L343) |
| VM-stack frame layout | [`zend_execute.c`](../../../../Zend/zend_execute.c#L4349) |
| User-frame initialization and current frame | [`zend_execute.c`](../../../../Zend/zend_execute.c#L4437) |
| Internal/user call boundaries | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L4127) |
| Exception publication and throwing opline | [`zend_exceptions.c`](../../../../Zend/zend_exceptions.c#L155) |
| Exception dispatch and unfinished-call cleanup | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8222) |
| Observer callback ABI and call boundaries | [`zend_observer.h`](../../../../Zend/zend_observer.h#L49), [`zend_observer.c`](../../../../Zend/zend_observer.c#L282) |
| Bailout catcher macros | [`zend.h`](../../../../Zend/zend.h#L273) |
| Nonlocal bailout implementation | [`zend.c`](../../../../Zend/zend.c#L1264) |
| Generator persistent frame and frozen calls | [`zend_generators.h`](../../../../Zend/zend_generators.h#L57) |
| Generator resume and parent-chain restoration | [`zend_generators.c`](../../../../Zend/zend_generators.c#L762) |
| Yield state materialization | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8442) |
| Fiber persistent execute-data state | [`zend_fibers.h`](../../../../Zend/zend_fibers.h#L101) |
| Fiber VM-state capture and restoration | [`zend_fibers.c`](../../../../Zend/zend_fibers.c#L105) |
| GC destructor invocation and fiber-aware cleanup | [`zend_gc.c`](../../../../Zend/zend_gc.c#L1871) |

## Validation

From the repository root:

```sh
python3 scripts/native/semantics/validate-frame-contract.py --check
python3 -m unittest discover -s tests/native/semantics/contracts/frames -p 'test_*.py' -v
python3 -m json.tool docs/native-engine/semantics/frames/frame-state.schema.json >/dev/null
python3 -m json.tool docs/native-engine/semantics/frames/frame-state.examples.json >/dev/null
```
