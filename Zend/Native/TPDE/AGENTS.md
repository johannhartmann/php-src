# TPDE backend instructions

These instructions apply to `Zend/Native/TPDE/**`.

- Keep all target-specific lowering, register assignment, code emission, and
  relocation handling in this subtree.
- Cross the Zend/native boundary through an explicit C ABI. Do not expose C++
  types in Zend public headers.
- Pin imported TPDE material to the revision in
  `ThirdParty/tpde/REVISION` (current upstream `master`) and record local
  changes in `ThirdParty/tpde/PATCHES.md`. No compatibility with older TPDE
  revisions.
- Emit frame-state and deoptimization metadata (ADR 0025) for every
  observation point and speculative guard of the generated code.
- Do not change vendored TPDE core code without updating its pin or documented
  patch provenance and running backend plus differential verification.
- Reject unsupported targets explicitly. Do not claim a target is supported
  solely because upstream TPDE contains code for it.
