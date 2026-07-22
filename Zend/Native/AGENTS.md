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
- Put architecture-specific lowering and target code only under `TPDE/`.
- Keep `MIR/`, `TPDE/`, and `Runtime/` as explicit architectural boundaries and
  read their `AGENTS.md` before changing them.
- Historical wave boundaries, model-only restrictions, generated profiles, and
  `codegen_eligible` barriers are not architectural requirements. Refactor or
  remove them when they prevent complete native execution.
- Extend the existing native build, PHPT, differential, and CI paths. Do not add
  per-wave gate frameworks, ownership manifests, receipts, ledgers, seals, or
  status infrastructure.
