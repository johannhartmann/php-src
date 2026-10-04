# LinuxX64 backend instructions

These instructions apply to `Zend/Native/TPDE/LinuxX64/**` in addition to
`Zend/Native/TPDE/AGENTS.md`.

Linux x86-64 is the primary target. Known pitfalls of the x64 backend:

- **Adaptor coverage.** When the common adaptor gains a node kind, operand
  form, or operand index, the backend must handle it. Missing lowerings show
  up as "TPDE failed to compile", or, when an operand is silently ignored, as
  TPDE's "found non-freed ValueAssignment" assertion.
- **Stack slots are negative.** TPDE indexes x64 frames downward
  (`FRAME_INDEXING_NEGATIVE`), so `allocate_stack_slot()` returns negative
  offsets. Reject `slot >= 0`.
- **Two-operand arithmetic.** A three-operand `d = a + b` is
  `mov(d, a, 8); ADD64rr d, b`. Emitting `ADD64rr d, b` alone keeps the old
  value of `d` and silently computes the wrong result.
- **Call arguments must not be fixed elsewhere.** Do not hold a later call
  argument in a `ScratchReg` while `CallBuilder` places earlier arguments;
  the allocator may have given it an earlier argument's SysV register
  (RDI, RSI, RDX, RCX, R8, R9), and evicting a fixed register asserts in
  `evict_reg`. Materialize such values directly in the argument register they
  are passed in, before allocating other scratches.
- **Cold blocks reload spilled parts.** When a guarded cold block stores a
  boxed value to the frame, reload parts whose assignment is `stack_valid()`
  from the stack instead of trusting the register.

- **Snippet register budget.** An EncodeGen snippet needs its scratch
  registers plus the held values free at once; gate inline paths on
  `unlocked_gp_registers()` or TPDE aborts in `select_reg_evict`.

Validate every change with the test tiers and compare the failing set against
the previous build (see the root `AGENTS.md`).
