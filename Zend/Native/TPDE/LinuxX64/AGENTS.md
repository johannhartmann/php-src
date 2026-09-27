# LinuxX64 backend instructions

These instructions apply to `Zend/Native/TPDE/LinuxX64/**` in addition to
`Zend/Native/TPDE/AGENTS.md`.

The DarwinA64 backend is the validated reference; the common adaptor evolves
with it first. Most LinuxX64 defects are incomplete or literal translations of
AArch64 code. Check every port against this list:

- **Adaptor drift.** When the adaptor gains a node kind, operand form, or
  operand index (for example `ZvalBoxedStore`, boxed boundary operands, the
  boxed `VALUE_COND_BRANCH` form), grep both backends. A name that DarwinA64
  handles and LinuxX64 does not is a missing lowering. Missing lowerings show
  up as "TPDE failed to compile", or, when an operand is silently ignored, as
  TPDE's "found non-freed ValueAssignment" assertion.
- **Stack slots are negative.** TPDE indexes x64 frames downward
  (`FRAME_INDEXING_NEGATIVE`), so `allocate_stack_slot()` returns negative
  offsets. Reject `slot >= 0`, never `slot < 0` as on AArch64.
- **Two-operand arithmetic.** AArch64 `ADDx d, a, b` becomes
  `mov(d, a, 8); ADD64rr d, b` on x64. Emitting `ADD64rr d, b` alone keeps
  the old value of `d` and silently computes the wrong result.
- **Call arguments must not be fixed elsewhere.** Do not hold a later call
  argument in a `ScratchReg` while `CallBuilder` places earlier arguments;
  the allocator may have given it an earlier argument's SysV register
  (RDI, RSI, RDX, RCX, R8, R9), and evicting a fixed register asserts in
  `evict_reg`. Materialize such values directly in the argument register they
  are passed in, before allocating other scratches.
- **Cold blocks reload spilled parts.** When a guarded cold block stores a
  boxed value to the frame, reload parts whose assignment is `stack_valid()`
  from the stack instead of trusting the register.

Validate every change with the native and partial-application PHPTs, and
compare the failing set against the previous build (see the root
`AGENTS.md`).
