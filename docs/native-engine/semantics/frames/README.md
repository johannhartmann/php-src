# Native frame semantics

These contracts define the frame layout, safepoints, bailout, exceptions,
suspension and resume of native code under ADR 0025: what each observation
point reads, where live values are, and how control continues.

## Contract map

- [Frame contract](frame-contract.md): physical frame and slot layout,
  ownership, roots, and lifecycle.
- [Safepoint contract](safepoint-contract.md): canonicalization boundaries and
  exact opline state before and after each boundary.
- [Resume ABI](resume-abi.md): exception, bailout, destructor reentry,
  generator/fiber suspension, and single-entry native resume.
- [Frame-state schema](frame-state.schema.json): required machine-readable
  fields for one canonical frame.
- [Frame-state examples](frame-state.examples.json): valid normal-call,
  exception, destructor, generator, and fiber states plus negative cases.
- [ADR 0025](../../adr/0025-native-canonical-execution.md) and
  [ADR 0010](../../adr/0010-bailout-exception-suspend-resume-abi.md): the
  architecture decisions behind them.

## Binding invariants

1. Native state is canonical. For every call that can reach an observation
   point, the frame header, arguments and parameter CVs are always current in
   its Zend `zend_execute_data` frame, and `EX(opline)` is stored before
   every transition into C; other CVs and temporaries live in registers or
   native stack slots and are written to their slots only where an
   observation point reads them.
2. Every observation point has a verified machine frame-state map naming the
   location of each live PHP value, root and cleanup obligation, and publishes
   exactly the state its boundary class reads before control crosses it.
3. Exceptions use `EG(exception)` plus explicit native control flow; a
   native callee reports one to its native caller with the
   `ZEND_NATIVE_EXCEPTION` status. Bailout is a nonlocal transfer.
4. A generator suspension moves every live value into the persistent,
   rooted generator frame; a fiber switch keeps its native activations on the
   fiber's own C stack. Generator and deoptimization resumes enter the
   function's single native entry, version-checked and addressed by resume
   ID.
5. Missing frame or resume metadata prevents code-version publication. There is
   no production VM fallback.

## Zend source anchors

These anchors identify the existing semantics this contract preserves:

| Subject | Source |
|---|---|
| `zend_execute_data` fields and call flags | [`zend_compile.h`](../../../../Zend/zend_compile.h#L646) |
| Frame-slot and argument address calculation | [`zend_compile.h`](../../../../Zend/zend_compile.h#L719) |
| VM-stack call allocation and cleanup | [`zend_execute.h`](../../../../Zend/zend_execute.h#L376) |
| VM-stack frame layout | [`zend_execute.c`](../../../../Zend/zend_execute.c#L4385) |
| User-frame initialization and current frame | [`zend_execute.c`](../../../../Zend/zend_execute.c#L4472) |
| Internal/user call boundaries | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L4129) |
| Exception publication and throwing opline | [`zend_exceptions.c`](../../../../Zend/zend_exceptions.c#L156) |
| Exception dispatch and unfinished-call cleanup | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8252) |
| Observer callback ABI and call boundaries | [`zend_observer.h`](../../../../Zend/zend_observer.h#L49), [`zend_observer.c`](../../../../Zend/zend_observer.c#L287) |
| Bailout catcher macros | [`zend.h`](../../../../Zend/zend.h#L275) |
| Nonlocal bailout implementation | [`zend.c`](../../../../Zend/zend.c#L1282) |
| Generator persistent frame and frozen calls | [`zend_generators.h`](../../../../Zend/zend_generators.h#L60) |
| Generator resume and parent-chain restoration | [`zend_generators.c`](../../../../Zend/zend_generators.c#L836) |
| Yield state materialization | [`zend_vm_def.h`](../../../../Zend/zend_vm_def.h#L8485) |
| Fiber persistent execute-data state | [`zend_fibers.h`](../../../../Zend/zend_fibers.h#L100) |
| Fiber VM-state capture and restoration | [`zend_fibers.c`](../../../../Zend/zend_fibers.c#L111) |
| GC destructor invocation and fiber-aware cleanup | [`zend_gc.c`](../../../../Zend/zend_gc.c#L1874) |

## Validation

The schema's enums must equal the catalog macros in
`Zend/Native/MIR/zend_mir_frame_state.h` (`scripts/native/mir/check-contract.py`
checks them), and the examples must validate against the schema with their
expected errors. From the repository root:

```sh
python3 scripts/native/semantics/validate-frame-contract.py --check
python3 -m unittest discover -s tests/native/semantics/contracts/frames -p 'test_*.py' -v
python3 scripts/native/mir/check-contract.py --check
```
