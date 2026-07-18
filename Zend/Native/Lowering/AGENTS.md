# W03 lowering rules

- Keep the lowering source view independent of PHP runtime structs and raw
  pointers.
- Accept an opcode only when every proof named by the generated W03 profile is
  present. Missing or contradictory facts are a hard failure.
- Providers must have disjoint source-opcode claims and deterministic ordering.
- Lowering results are failure-atomic: an MIR module may escape only after
  finalization and both verifier stages succeed.
- Do not add a VM or interpreter fallback.
