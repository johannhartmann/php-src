# Native MIR instructions

These instructions apply to `Zend/Native/MIR/**`.

- Maintain exactly one canonical, architecture-independent ZNMIR/PHP-MIR.
- Express control flow, side effects, ownership, exceptional edges, and frame
  states explicitly; do not recover them from target-specific conventions.
- Keep target registers, instruction encodings, and platform calling
  conventions out of the MIR.
- Verify every MIR unit before lowering. A verifier must reject malformed
  control flow, invalid ownership, incomplete frame states, and unsupported
  effects rather than silently repairing them.
- Keep the existing structural verifier coherent with executable MIR changes
  and prove observable semantics through the existing direct execution,
  differential, PHPT, sanitizer, and target tests. Do not create per-wave
  verifier frameworks, gate manifests, receipts, or seal machinery.
