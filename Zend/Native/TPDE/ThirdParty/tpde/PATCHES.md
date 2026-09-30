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
