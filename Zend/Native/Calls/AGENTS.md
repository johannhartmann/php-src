# Native call semantics

This subtree owns the target-neutral call representation used by executable
native user, internal, and method calls.

- Lower a complete reachable INIT/SEND/DO sequence atomically, including named
  arguments, defaults, extra arguments, unpacking, variadics, references, and
  refcounted values.
- Preserve parameter and return-by-reference rules, argument-container cleanup,
  observers, exceptions, bailout, reentry, and result ownership exactly.
- Do not use function-name allowlists or keep valid calls model-only. A call
  must execute or fail for its real PHP error.
- Persistent records and stable identities remain pointer-free. Process-local
  resolution and Runtime bindings may inspect `zend_function *`, but no process
  address may enter MIR.
- Keep target ABI mechanics in `TPDE/` and Zend call-frame semantics in
  `Runtime/`; do not add a VM fallback, opcode handler call, or MIR interpreter.
- Calls between compiled PHP functions use the native calling convention of
  ADR 0025: the callee frame lives on the VM stack with an eagerly written
  header, arguments go to the parameter slots and may additionally travel in
  registers, and results return in registers. Everything else in the Zend
  frame is reconstructed from frame-state metadata when the callee's state
  becomes observable and at every boundary to internal functions, callbacks,
  reflection and observers.
- Extend the existing execution and PHPT coverage without introducing call
  gates, profiles, manifests, receipts, or ledgers.
