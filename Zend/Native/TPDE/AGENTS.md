# TPDE backend instructions

These instructions apply to `Zend/Native/TPDE/**`.

- Keep all target-specific lowering, register assignment, code emission, and
  relocation handling in this subtree.
- Cross the Zend/native boundary through an explicit C ABI. Do not expose C++
  types in Zend public headers.
- Pin imported TPDE material to reference commit
  `338d41890e424b058e2053b6a5787e1348e3dd57` and record its provenance.
- Do not change vendored TPDE core code without an explicit ownership decision,
  an updated pin or patch record, and backend plus differential verification.
- Reject unsupported targets explicitly. Do not claim a target is supported
  solely because upstream TPDE contains code for it.
