# ZNMIR lowering

Lowering turns a Zend op_array with its SSA and type inference into canonical
ZNMIR. It consumes process-local source views, dispatches every opline through
a deterministic provider registry, and emits MIR through the mutator contract.
Typing rules follow ADR 0024; frame states and speculation follow ADR 0025.

An opline that no provider lowers fails the function with a stable diagnostic
and no module; there is no VM fallback.
