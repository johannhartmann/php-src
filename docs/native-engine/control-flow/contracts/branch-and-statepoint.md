# W04 branch and edge-statepoint contract

Canonical MIR fixes `COND_BRANCH` successor 0 as true and successor 1 as false.
Zend source order is retained in the source edge table:

| Zend opcode | source successor 0 | source successor 1 | MIR permutation |
| --- | --- | --- | --- |
| `JMPZ`, `JMPZ_EX` | false jump target | true fallthrough | `0 -> 1`, `1 -> 0` |
| `JMPNZ`, `JMPNZ_EX` | true jump target | false fallthrough | `0 -> 0`, `1 -> 1` |

`JMP` has one explicit-jump successor and lowers to `BRANCH`.

Conditional branches are accepted only with an exact scalar truth proof, no
reference-count, destructor, exception, bailout, reentry, or observer path,
and a complete source-backed successor mapping. `_EX` variants additionally
require an exact result-SSA proof.

An edge flagged `interrupt_boundary` requires exactly one edge statepoint
before its destination. The source-to-MIR edge mapping identifies that
instruction. A missing, extra, or misplaced statepoint is `[MIRV0605]`.
