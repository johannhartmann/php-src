# Zend native-engine instructions

These instructions apply to `Zend/Native/**` in addition to the repository root
instructions.

- Keep public Zend headers C-compatible. Do not expose C++ types, templates, or
  exception semantics through a Zend C header.
- Keep C++ implementation details behind explicit C ABI boundaries.
- Do not implement a VM opcode-dispatch loop as a native runtime helper or a
  production fallback path.
- Preserve Zend value, call, bailout, and lifetime semantics. Require an ADR and
  focused compatibility tests before changing an ABI boundary.
- Follow ADR 0025 (native state canonical, lazy Zend-frame reconstruction,
  native call ABI, speculation with deoptimization into generic native code,
  guarded inlining). Every observation point needs complete frame-state
  metadata; a missing or wrong frame state is a correctness bug, not a
  performance issue.
- Put architecture-specific lowering and target code only under `TPDE/`.
- Keep `MIR/`, `TPDE/`, and `Runtime/` as explicit architectural boundaries.
  Read the `AGENTS.md` of `Calls/`, `Lowering/`, `MIR/`, `Runtime/`, `TPDE/`
  (and `TPDE/LinuxX64/`) and `Values/` before changing code there.
- [`TPDE.md`](../../TPDE.md) describes the pipeline, the source tree, the
  build profiles and the test tiers.
- Extend the existing native build, PHPT, differential, and CI paths. Do not add
  staged gate frameworks, ownership manifests, receipts, ledgers, seals, or
  status infrastructure.
