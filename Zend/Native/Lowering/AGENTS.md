# Native lowering rules

- Keep persistent lowering records independent of PHP runtime pointers. A
  process-local source view may inspect Zend structures while lowering.
- Lower every valid operation in the active language scope to executable MIR.
  Historical generated profiles, provider allowlists, wave ownership, and
  `codegen_eligible` flags are not acceptance gates.
- Validate the facts required by the operation itself. Missing or contradictory
  semantic facts fail atomically; they must not be replaced by permissive
  defaults, model-only success, or VM fallback.
- Lower complete Zend constructs atomically, including calls, `OP_DATA`, ropes,
  foreach pairs, exception regions, and other coupled opcode sequences.
- Providers must have deterministic, non-conflicting claims. Source successor,
  predecessor, and PHI input order are semantic and must not be reconstructed
  from pointer order.
- Keep source-to-MIR mappings and Zend pointers process-local. Persistent MIR
  records contain stable IDs only.
- Preserve frame states, roots, cleanup obligations, exceptional edges, and
  failure atomicity until executable code has been produced.
- Extend existing direct execution tests and CI. Do not introduce new wave
  profiles, ownership manifests, gate frameworks, receipts, ledgers, or status
  dashboards.
- Do not add a VM, opcode-dispatch helper, interpreter, or production fallback.
