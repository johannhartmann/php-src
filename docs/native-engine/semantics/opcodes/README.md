# W01 Zend opcode coverage matrix

This directory inventories every executable Zend opcode at source commit
`b7c524a19fa815799a858b98d39f176ca88648b1`. The W01 wave base remains
`dc6e34b56846c38dc2475d6c962c2b9b7ada6df4`; the source pin advances when
upstream adds executable opcodes. The inventory changes no PHP behavior and
does not claim that the planned native engine exists.

## Files

- `opcode-matrix.schema.json` defines the combined machine-readable contract.
- `opcode-overrides.json` contains reviewable semantic judgments and source
  anchors; it is never inferred from opcode names during normal generation.
- `opcode-matrix.json` and `opcode-matrix.csv` are timestamp-free generated
  views with identical rows.
- `scripts/native/semantics/generate-opcode-matrix.py` extracts canonical
numbers, handler operand forms, and type-specialized variants, validates every
override fail-closed, and emits both views.

`ZEND_VM_LAST_OPCODE` is recorded as a sentinel, not an executable opcode.
Reserved numeric holes are recorded separately. `ZEND_OP_DATA` is executable
metadata and is extracted from `ZEND_VM_DEFINE_OP` even though it has no normal
handler body.

## Source model

The generator reads and hashes all sources mandated by W01-A. Its structural
extraction follows the base-handler and type-specialization grammar implemented
by `Zend/zend_vm_gen.php`. Direct `ZEND_VM_DISPATCH_TO_HANDLER` and
`ZEND_VM_DISPATCH_TO_HELPER` edges are extracted as operand metadata. An
override must cite the complete definition range of every direct dispatch
target, so a wrapper or fast path cannot hide behavior in an uncited alias or
slow-path helper. The `zend_op` layout in `Zend/zend_compile.h` and
use/def separation in `Zend/Optimizer/zend_ssa.h` explain why result semantics
cannot be inferred from handler operands and therefore live in overrides.

Function-JIT evidence is anchored to explicit opcode cases in the
`zend_jit()` compiler in `ext/opcache/jit/zend_jit.c`. Trace-JIT evidence is
anchored to explicit opcode cases in the `zend_jit_trace()` compiler in
`ext/opcache/jit/zend_jit_trace.c`. Helper-only, analysis-only, and function-JIT
tail-handler occurrences do not establish full support and are classified as
`partial`. `function`, `trace`, and `both` mean an explicit lowering exists in
the named mode; they do not claim coverage of every operand type or runtime
guard combination.

Effect, domain, ownership, barrier, reference, observer, interrupt, and special-
case claims come from `opcode-overrides.json`. Every entry cites a line-bounded
source reference whose symbol is validated during generation. Conservative
`conditional` means the cited handler contains a path with that effect; it is
not a substitute for an unresolved annotation.

An opcode is tagged `zts` when its handler accesses executor-global (`EG`)
state, which is thread-local in ZTS builds. Fiber tagging is narrower: it marks
only a handler with an explicit fiber interaction, rather than assuming that
every call which could eventually invoke `Fiber` is itself a fiber opcode.

The `planned_znmir_lowering` values are stable planning families, not W02 MIR
operations. Both N0 target fields describe planning state only; W01 does not
claim x86-64 or AArch64 implementation support.

## Regeneration

From the repository root:

```sh
python3 scripts/native/semantics/generate-opcode-matrix.py
python3 scripts/native/semantics/generate-opcode-matrix.py --check
python3 -m unittest discover \
  -s tests/native/semantics/contracts/opcodes -p 'test_*.py' -v
```

For deterministic comparison, generate into two empty directories and compare
both files:

```sh
python3 scripts/native/semantics/generate-opcode-matrix.py --output-dir /tmp/opcodes-a \
  --overrides docs/native-engine/semantics/opcodes/opcode-overrides.json
python3 scripts/native/semantics/generate-opcode-matrix.py --output-dir /tmp/opcodes-b \
  --overrides docs/native-engine/semantics/opcodes/opcode-overrides.json
cmp /tmp/opcodes-a/opcode-matrix.json /tmp/opcodes-b/opcode-matrix.json
cmp /tmp/opcodes-a/opcode-matrix.csv /tmp/opcodes-b/opcode-matrix.csv
```

## Review workflow and limits

When a Zend source changes, regenerate only after reviewing the changed handler,
its callees, its function/trace JIT paths, and the corresponding override.
Generation fails for new or removed opcodes, duplicate names/numbers, stale or
out-of-range source anchors, missing fields, invalid shared-vocabulary IDs,
prohibited placeholders, and stale checked-in output.

The matrix is a source-backed W01 planning contract. It does not replace PHPT or
differential execution, prove every transitive callee effect, or make a backend
support promise. Those limits require conservative annotations and focused tests
rather than silent normalization or guessed support.
