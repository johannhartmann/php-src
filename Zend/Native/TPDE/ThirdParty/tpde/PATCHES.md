# Local integration patch

The imported TPDE sources are pinned by `REVISION`. PHP carries one additive,
target-neutral API patch in `include/tpde/Assembler.hpp`: read-only access to
the finalized section list and relocation spans. The Darwin in-memory mapper
needs this information to lay out TPDE output, apply AArch64 relocations, and
enforce per-section final permissions without routing Apple code through the
ELF object model.

The patch does not alter analysis, liveness, register allocation, instruction
selection, encoding, section contents, symbols, or relocation creation.
