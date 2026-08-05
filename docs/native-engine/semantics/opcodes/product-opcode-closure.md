# Current native product opcode closure

This proof covers the 212 active opcode numbers in the current
`Zend/zend_vm_opcodes.h`. It is generated exclusively from current product
sources. Historical wave profiles, reclassification files, planning matrices,
and `codegen_eligible` metadata are not inputs.

The checked artifact is
`product-opcode-closure.json`. It records SHA-256 hashes for every source used
to classify an opcode, plus an aggregate digest of the native C/C++ product
tree scanned for forbidden VM acceptance. A product-source change therefore
makes `--check` fail until the proof is regenerated and reviewed.

## Classification

The verifier follows the production `user_opcode_target()` switch and its
`zend_mir_w12_executable_opcode()` fallback. A fallback is accepted only when
the mapped MIR opcode is executable and has a bounded runtime-helper mapping.
Direct target kinds must be implemented by both supported TPDE targets.

`OP_DATA` is classified separately because it is not an independent landing:
lowering consumes it atomically as the auxiliary operand of its owner opcode,
and TPDE marks its source position as data.

The three executor-special opcodes have explicit semantic classifications:

| Opcode | Product classification | Current semantic path |
| --- | --- | --- |
| `ZEND_HANDLE_EXCEPTION` | `native_exception_route` | The compiler maps Zend exception handlers to MIR exception edges; TPDE emits target-native branches to the recorded exception block. |
| `ZEND_USER_OPCODE` | `native_user_opcode_callback` | A bounded helper invokes the registered callback, interprets its action, and resolves `ENTER` through the native entry registry. |
| `ZEND_CALL_TRAMPOLINE` | `native_call_trampoline_normalization` | The bounded dynamic-call path normalizes magic-call trampolines to `__call`/`__callStatic` and continues through native user/internal entries. |

Every special classification contains current file, line, and source-anchor
evidence. Contract tests delete these anchors to prove that the verifier fails
closed instead of silently retaining a label.

## Hard-zero gates

Generation succeeds only when all of these values are zero:

- active opcodes without a product path;
- model-only paths;
- deferred paths;
- unsupported valid paths;
- `zend_vm_call_opcode_handler()` calls;
- `execute_ex()` fallbacks;
- `opline->handler` calls.

The VM-interface scan covers native product C, C++, header, and include files;
it is not limited to the files that happen to classify individual opcodes.

## Commands

Regenerate the current proof:

```sh
python3 scripts/native/verify-opcode-closure.py --write
```

Verify that the checked artifact still matches the product sources:

```sh
python3 scripts/native/verify-opcode-closure.py --check
```

Run the fail-closed contract tests:

```sh
python3 -m unittest -v \
  tests/native/semantics/contracts/opcodes/test_product_opcode_closure.py
```
