# TPDE source provenance

The analysis covers repository `https://github.com/tpde2/tpde.git` at the single
commit:

```text
338d41890e424b058e2053b6a5787e1348e3dd57
```

It makes no statement about any other TPDE revision. The checkout was
detached at that commit and clean. Reproduce the identity check without changing
the checkout:

```sh
git -C "$TPDE_SOURCE_DIR" rev-parse HEAD
git -C "$TPDE_SOURCE_DIR" status --short
python3 scripts/native/semantics/validate-tpde-gap.py \
  --tpde "$TPDE_SOURCE_DIR" --check
```

The first command must print the commit above and the second must print nothing.
The validator additionally checks every recorded path, line range, and symbol.
`TPDE_SOURCE_DIR` is runtime input and is deliberately absent from committed
artifacts.

## Reviewed source set

The review read these files in full or in the source regions relevant to the
inventory:

```text
README.md
docs/tpde/adaptor.md
docs/tpde/overview.md
docs/tpde/compiler-ref.md
tpde/include/tpde/IRAdaptor.hpp
tpde/include/tpde/Compiler.hpp
tpde/include/tpde/CompilerBase.hpp
tpde/include/tpde/ValueAssignment.hpp
tpde/include/tpde/AssignmentPartRef.hpp
tpde/include/tpde/AssemblerElf.hpp
tpde/include/tpde/ElfMapper.hpp
tpde/src/ElfMapper.cpp
tpde/include/tpde/FunctionWriter.hpp
tpde/include/tpde/x64/CompilerX64.hpp
tpde/include/tpde/arm64/CompilerA64.hpp
tpde/CMakeLists.txt
CMakeLists.txt
```

The exact same list is enforced by `required-capabilities.json` and the custom
validator.

## Build and target boundary

Top-level `CMakeLists.txt:11-13` requires C++20. `CMakeLists.txt:37-50` defaults
`TPDE_ENABLE_EH` to off and selects no C++ exception handling (`-fno-exceptions`
on the non-MSVC path). `tpde/CMakeLists.txt:22-29` configures spdlog consistently
with that mode. This is C++ language exception handling; it is distinct from
emitting DWARF unwind/EH-frame data for generated code.

The pinned README and ELF assembler types identify x86-64 and AArch64 ELF
backends. For PHP, Linux/ELF x86-64 is the only primary release target in this
analysis. AArch64 backend existence is evidence for analysis only; it is not a
PHP support claim. The AArch64 cache-publication and branch-veneer gaps remain
explicit entries in the inventory.
