# Native value semantics

This subtree owns the single target-neutral value implementation used by native
execution. It must represent and execute canonical zvals, references, aliases,
ownership transfers, separation, cleanup, strings, arrays, iterators, and
argument containers. It is not a model-only layer.

- Replace or simplify historical W06 records when they obstruct executable
  semantics; do not create a second parallel value system.
- Valid value and container operations in the active implementation scope must
  lower to executable MIR. Modeling-only success, `codegen_eligible` barriers,
  and deliberate compile rejection are not implementations.
- Keep persistent MIR identity pointer-free and target-neutral. Process-local
  runtime bindings may use Zend pointers while they are alive.
- Preserve exact Zend addref, move, release, destructor, reference, COW, GC-root,
  warning, exception, and bailout ordering. Indirect slots and reference cells
  remain distinct concepts.
- Use bounded Runtime helpers for Zend storage and container operations when
  inline expansion is inappropriate. Helpers must never dispatch VM opcodes or
  provide a production VM fallback.
- Keep multi-opcode constructs such as `OP_DATA`, ropes, calls, and iterator
  pairs atomic during lowering and failure.
- Production sources are C11. C ABI headers must also compile as C++20.
- Extend direct execution tests and the existing native test matrix; do not add
  wave profiles, ownership manifests, receipts, or value-specific gate systems.
