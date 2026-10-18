# TPDE backend

This directory holds the TPDE code generators of the native engine:
`Common/` (planning, IR adaptor, shared code generator, image format, layout
profile), `LinuxX64/` (the primary target), `DarwinA64/` (Darwin arm64 with
a PHP-owned in-memory A64 assembler, since upstream TPDE only emits ELF for
A64), `EncodeGen/` (snippet sources for the checked-in encoders) and
`ThirdParty/tpde/` (vendored TPDE, Fadec and Disarm).

[`TPDE.md`](../../../TPDE.md) explains TPDE, its integration and the build.
`ThirdParty/tpde/REVISION` pins the upstream revisions and
[`ThirdParty/tpde/PATCHES.md`](ThirdParty/tpde/PATCHES.md) lists every local
change. Updates move the pin to newer upstream `master` revisions;
compatibility with older revisions is not kept.

The build is network-independent: configure regenerates the Fadec encoding
tables from the vendored table and generator. C++20 code is compiled without
exceptions and RTTI, and neither backend can fall back to the Zend VM.
