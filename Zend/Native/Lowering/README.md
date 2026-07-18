# ZNMIR lowering contract

This directory freezes the target-neutral W03 lowering boundary. The boundary
consumes immutable, pointer-free source and fact records, dispatches through a
deterministic provider registry, and emits canonical ZNMIR through the mutator
contract.

The generated opcode profile is the acceptance authority. A conditional opcode
is lowerable only when all listed proofs are available. Otherwise lowering must
return a stable diagnostic and no module.

Implementation belongs to the W03 specialist tracks. These headers define
interfaces and stable numeric identities only.
