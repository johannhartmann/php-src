# W01 semantic contracts

W01 inventories existing PHP semantics and freezes the contracts required by
the MIR and native-engine waves. It does not change PHP runtime behavior, the
public ABI, or the build system.

## Normative inputs

- [Opcode inventory](opcodes/README.md) records every executable Zend opcode,
  source evidence, effects, ownership actions, and planned lowering family.
- [Effect and ownership model](effects/README.md) defines the registered effect,
  memory-domain, ownership, guard, and barrier vocabularies.
- [Frame contracts](frames/README.md) define observable frame state, safepoints,
  bailout, exception, suspend, and single-entry resume behavior.
- [TPDE analysis](../tpde/README.md) classifies backend capabilities at the
  pinned TPDE source commit.
- [Semantic oracle corpus](../../../tests/native/semantics/README.md) maps every
  required opcode family and critical semantic risk to deterministic fixtures.
- [Shared semantic vocabulary](vocabulary.md) freezes the IDs
  shared by the opcode inventory and effect model.
Run the complete cross-check from the repository root:

```bash
python3 scripts/native/semantics/validate-w01.py --check
```

The validator checks the live normative inputs directly; it does not generate
or require a checked-in coverage report.
