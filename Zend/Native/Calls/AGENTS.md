# Native call semantics

This subtree owns the target-neutral call representation used by executable
native user, internal, and method calls. It is not restricted to the historical
W05 positional-scalar model.

- Lower a complete reachable INIT/SEND/DO sequence atomically, including named
  arguments, defaults, extra arguments, unpacking, variadics, references, and
  refcounted values.
- Preserve parameter and return-by-reference rules, argument-container cleanup,
  observers, exceptions, bailout, reentry, and result ownership exactly.
- Do not use function-name allowlists or keep valid calls model-only. A call in
  the active implementation scope must execute or fail for its real PHP error.
- Persistent records and stable identities remain pointer-free. Process-local
  resolution and Runtime bindings may inspect `zend_function *`, but no process
  address may enter MIR.
- Keep target ABI mechanics in `TPDE/` and Zend call-frame semantics in
  `Runtime/`; do not add a VM fallback, opcode handler call, or MIR interpreter.
- Extend the existing execution and PHPT coverage without introducing per-wave
  call gates, profiles, manifests, receipts, or ledgers.
