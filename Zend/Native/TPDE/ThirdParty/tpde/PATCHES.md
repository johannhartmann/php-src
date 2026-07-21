# Local integration patches

The imported TPDE sources are pinned by `REVISION`. PHP carries these additive,
target-neutral integration patches:

- `include/tpde/Assembler.hpp` exposes read-only access to the finalized section
  list and relocation spans. The Darwin in-memory mapper needs this information
  to lay out TPDE output, apply AArch64 relocations, and enforce per-section
  final permissions without routing Apple code through the ELF object model.
- `include/tpde/util/AddressSanitizer.hpp` supplies the conventional false
  fallback for Clang's `__has_feature` macro so the header also preprocesses
  with GCC. GCC's `__SANITIZE_ADDRESS__` detection remains unchanged.

The patches do not alter analysis, liveness, register allocation, instruction
selection, encoding, section contents, symbols, or relocation creation.
