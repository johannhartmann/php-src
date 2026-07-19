# W04 lowering rules

- Keep the lowering source view independent of PHP runtime structs and raw
  pointers.
- Accept an opcode only when every proof named by the generated W03 profile is
  present. Missing or contradictory facts are a hard failure.
- W04 providers must additionally satisfy every CFG, branch-order, PHI-order,
  reducibility, and edge-statepoint proof named by the generated W04 profile.
- Providers must have disjoint source-opcode claims and deterministic ordering.
- W03 lowering results remain failure-atomic after finalization and verifier
  stages 1 and 2. W04 results may escape only after verifier stage 3 also
  succeeds while the source view and process-local control-flow map are alive.
- Source successor order, predecessor order, and PHI input order are semantic.
  Never sort or reconstruct these tables from pointers.
- Keep source-to-MIR mappings process-local. Persistent MIR records must not
  contain source pointers.
- Do not add a VM or interpreter fallback.
