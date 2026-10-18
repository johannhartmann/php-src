# ZNMIR lowering

Lowering turns a Zend op_array with its SSA and type inference into canonical
ZNMIR (`zend_mir_lower_zend_op_array()`, and
`zend_mir_lower_typed_zend_op_array()` for the typed tier in
`zend_mir_lowering_zend.h`). It consumes process-local source views
(`Frontend/`), dispatches every opline through a deterministic provider
registry (`Core/`), and emits MIR through the mutator contract. Typing rules
follow [ADR 0024](../../../docs/native-engine/adr/0024-typed-lowering-tier.md);
frame states and speculation follow
[ADR 0025](../../../docs/native-engine/adr/0025-native-canonical-execution.md).

An opline that no provider lowers fails the function with a stable diagnostic
and no module; there is no VM fallback. [`TPDE.md`](../../../TPDE.md)
section 2.4 places lowering in the compilation pipeline.
