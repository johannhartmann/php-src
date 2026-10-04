# TPDE backend

The backend follows current upstream TPDE: `ThirdParty/tpde/REVISION` pins
`master` commit `9779acf4ada3736e779391da1e4b3369dba08024`, and
`ThirdParty/tpde/PATCHES.md` lists every local change. Updates move the pin to
newer upstream revisions; compatibility with older revisions is not kept.

The build is network-independent. The exact TPDE and Fadec revisions and their
upstream licenses are stored in `ThirdParty/tpde`. Linux x86-64 compiles the
actual Fadec encoder vendored from TPDE's pinned dependency; its generated
64-bit encoding tables are reproduced during configure from the vendored table
and generator. Darwin uses a PHP-owned in-memory A64 emitter because upstream
TPDE only provides an ELF A64 assembler. No ELF output is presented as Darwin
code.

Linux x86-64 is the primary target. C++20 stays below
`Zend/Native/TPDE`, exceptions and RTTI are disabled, and neither backend can
fall back to the Zend VM.
