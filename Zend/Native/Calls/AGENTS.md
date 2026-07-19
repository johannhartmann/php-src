# W05 call-model instructions

This subtree models calls; it does not execute them.

- Publish a call only after the complete reachable INIT/SEND/DO sequence has
  been planned and validated atomically.
- W05 accepts only exact direct same-script user functions with positional
  by-value non-refcounted scalar arguments.
- Records and stable identity are pointer-free. Process-local resolution may
  inspect `zend_function *`, but no address may enter MIR.
- Keep runtime binding, exception/bailout/reentry cleanup, observers, result
  ownership, internal handlers, and C-ABI interoperability as explicit debts.
- Do not add a VM fallback, MIR interpreter, target emission, or Stage 4.
