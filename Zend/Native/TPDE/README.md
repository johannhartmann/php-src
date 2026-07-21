# TPDE backend

The W06 baseline is pinned to upstream TPDE commit
`d19a36fe9a3657f36b6bf6777d87c5cec0cfce5b`. The previous design reference
was `338d41890e424b058e2053b6a5787e1348e3dd57`.

The build is network-independent. The exact TPDE and Fadec revisions and their
upstream licenses are stored in `ThirdParty/tpde`. Linux x86-64 compiles the
actual Fadec encoder vendored from TPDE's pinned dependency; its generated
64-bit encoding tables are reproduced during configure from the vendored table
and generator. Darwin uses a PHP-owned in-memory A64 emitter because upstream
TPDE only provides an ELF A64 assembler. No ELF output is presented as Darwin
code.

Only the two W06 targets are implemented. C++20 stays below
`Zend/Native/TPDE`, exceptions and RTTI are disabled, and neither backend can
fall back to the Zend VM.
