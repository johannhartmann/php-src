# W04 ownership contract

W03 split provider registration, frontend source extraction, scalar providers,
straight-line lowering, and bridge wiring across tracks. In W04 those seams
meet at concrete shared paths:

- `Zend/Native/Lowering/Core/zend_mir_lowering_providers.c` registers all
  providers;
- `Zend/Native/Lowering/Frontend/zend_mir_zend_source.c` owns the Zend-backed
  source view;
- `Zend/Native/Lowering/Scalar/**` and
  `Zend/Native/Lowering/StraightLine/**` must consume multi-block state;
- `ext/native_mir_test/native_mir_test.c` exposes the single test bridge;
- `ext/native_mir_test/config.m4` wires every production source.

To remove those integration conflicts, one production specialist owns all four
lowering subtrees plus the new control-flow implementation and unit tests. One
evidence specialist owns the bridge implementation, corpus, fuzzing,
differential runner, and W04 environment profiles. The integration gate alone
owns `config.m4`, CI, validation/report scripts, integration tests, coverage
report, and wave status.

`w04-ownership.json` is the source of task paths. H and P contract files are
reserved against both specialists. `check-ownership.py` rejects overlapping
patterns and rejects every real `base..head` path outside the selected task,
including integration-only paths.

`w04-source-files.json` is the exact source list input for the later
`config.m4` integration. The integration gate must generate its W04 additions
from that manifest instead of discovering source files with a wildcard.

H also closes two prerequisite gaps which cannot be delegated to either
specialist: the canonical MIR Core owns persistent process-local CFG edge
storage and the W03 hard-gate runner owns its binary-independent self-test.
Those files and their focused tests are contract-reserved. W02 inventory
attestation excludes the W04 `MIR/ControlFlow` subtree, and the W03 report
attests its frozen path set plus W03 profile, blocker, and corpus evidence.
Consequently additive W04 implementation and bridge changes cannot force
either specialist to rewrite an earlier wave's shared coverage report.
