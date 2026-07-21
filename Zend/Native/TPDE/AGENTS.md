# TPDE backend instructions

These instructions apply to `Zend/Native/TPDE/**`.

- Keep all target-specific lowering, register assignment, code emission, and
  relocation handling in this subtree.
- Cross the Zend/native boundary through an explicit C ABI. Do not expose C++
  types in Zend public headers.
- Pin imported TPDE material to reference commit
  `d19a36fe9a3657f36b6bf6777d87c5cec0cfce5b` and record its provenance.
- Do not change vendored TPDE core code without updating its pin or documented
  patch provenance and running backend plus differential verification.
- Reject unsupported targets explicitly. Do not claim a target is supported
  solely because upstream TPDE contains code for it.
