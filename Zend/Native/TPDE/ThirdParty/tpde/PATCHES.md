# Local integration patches

The imported TPDE sources are pinned by `REVISION` to upstream `master`
9779acf; the library sources under `include/` and `src/` equal that revision
except for the patches below. Fadec and Disarm are pinned to the revisions
TPDE's submodules use. Newer upstream revisions replace the pin; this tree
keeps no compatibility with older TPDE revisions.

## Mapper access

- `include/tpde/Assembler.hpp` exposes read-only access to the finalized
  section list and relocation spans. The Darwin in-memory mapper needs this
  information to lay out TPDE output, apply AArch64 relocations, and enforce
  per-section final permissions without the ELF object model.
- `include/tpde/util/AddressSanitizer.hpp` supplies the conventional false
  fallback for Clang's `__has_feature` macro so the header also preprocesses
  with GCC.

## Fadec

- `fadec/encode2.c` widens shift operands to unsigned 32-bit values before
  shifting, removing signed-shift undefined behaviour UBSan reports.

## Scratch value parts

- `include/tpde/ValuePartRef.hpp` adds `ValuePart(RegBank, u32 size)`, an
  unassigned non-constant part used as call result and snippet output
  scratch.

## Jump threading (`include/tpde/FunctionWriter.hpp`, `src/FunctionWriter.cpp`, `src/x64/FunctionWriterX64.cpp`, `include/tpde/x64/CompilerX64.hpp`)

- `FunctionWriter` records the labels placed at the current offset; when
  `CompilerX64::generate_raw_jump` emits an unconditional jump there, those
  labels alias its target (`label_alias_jump`). `FunctionWriterX64::
  handle_fixups` resolves the fixups of jmp rel32 and jcc rel32 instructions
  through these aliases (`label_resolve_jump`, at most eight hops), so a jump
  to a block or label that only forwards goes straight to the final target,
  and turns a jump to the immediately following instruction into a NOP.
  `label_offset()` and RIP-relative operands are unchanged.

## Allocator and control flow (`include/tpde/CompilerBase.hpp`)

General corrections:

- `CompilerBase::generate_cond_branch` with identical targets emits an
  unconditional branch.
- `CompilerBase::generate_switch` reuses one PHI-moving label per target
  block, including the default block.
- Value-local assignment pointers are cleared between the functions of a
  multi-function adaptor, so a release build never carries a stale pointer
  into the next function.
- Long-lived multi-part values get a fixed register per part, as single-part
  values do.

Support for machine-code branches inside one IR instruction. The PHP emitter
still lowers some operations with a fast path and a slow path inside a single
IR instruction, which TPDE's CFG does not see. These patches keep such values
consistent; they become unnecessary once those operations are lowered with
IR-visible guard, cold and join blocks:

- `RetBuilder::ret_local_path` emits a return on an outlined local path
  without ending allocation for the hot continuation.
- A non-fixed register argument live beyond the entry block gets a canonical
  spill copy in the prologue.
- A value with remaining references is spilled before a branch even when its
  block liveness ends, and `free_reg` accepts discarding a modified register
  only once no reference remains.

## Cold code area (`include/tpde/FunctionWriter.hpp`, `src/FunctionWriter.cpp`, `include/tpde/x64/CompilerX64.hpp`, `include/tpde/CompilerBase.hpp`)

Code that rarely runs can be written out of the hot instruction stream while
it is compiled in place, so the allocator state stays that of its position:

- `FunctionWriterBase::begin_cold_area()`/`end_cold_area()` redirect writing
  to a per-function side buffer whose offsets start at
  `FunctionWriterBase::ColdAreaBase`; `more_space` grows that buffer and
  relocations recorded meanwhile are deferred (`reloc()`, and
  `CompilerBase::reloc_text`, which now writes through it).
- `append_cold_area()` moves the buffer behind the hot code of the function
  and translates label offsets, label fixups, jump tables and the deferred
  relocations; `translate_cold_offset()` maps other recorded offsets.
  `CompilerX64::finish_func` appends the area first and translates its
  return-patch offsets.
- `CompilerX64::generate_raw_jump` takes a fixup for a label in the other
  area (`label_needs_fixup()`). `next_block()` names the next block in
  layout order written to the same area, the one physically placed next
  (one past the last block if none), so no branch falls through into the
  other area; in the cold area `spill_before_branch` sees no fall-through
  successor.
- An adaptor that provides `block_is_cold(IRBlockRef)` has those blocks
  written to the cold area by `CompilerBase::compile_block`; they are still
  compiled in layout order, so the allocator state is that of their
  position.
