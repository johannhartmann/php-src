# TPDE capability analysis

This directory records the W01-D analysis of `tpde2/tpde` at exactly
`338d41890e424b058e2053b6a5787e1348e3dd57`. It is a design and contract
inventory, not TPDE integration code and not a claim that the native engine is
implemented.

The machine-readable source of truth is
`required-capabilities.json`. Its JSON Schema documents the interchange shape;
`scripts/native/semantics/validate-tpde-gap.py` is the authoritative repository
validator because it also checks the Git pin and source anchors.

Documents:

- `source-provenance.md` fixes the reviewed repository, revision, files, build
  mode, targets, and reproduction commands.
- `gap-analysis.md` explains the classifications and the boundaries between
  superficially similar capabilities.
- `statepoint-and-location-hooks.md` proposes generic backend hooks.
- `mapper-relocation-unwind.md` separates emission, live mapping, persistence,
  relocation, protection, cache, and unwind responsibilities.
- `upstream-and-patch-plan.md` breaks the necessary work into reviewable patches.

No capability classified as `tpde_extension`, `tpde_core_patch`, or `php_shim`
may be treated as available until its named implementation and tests exist.
AArch64 remains an analyzed, unreleased follow-on target.
