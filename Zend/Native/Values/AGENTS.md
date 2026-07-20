# W06 value-model contracts

This subtree owns the target-neutral W06 storage, reference, alias, ownership,
and separation contracts. Records must remain pointer-free and use stable MIR
IDs. W06 models transitions but does not execute reference binding, releases,
destructors, container clones, calls, or target code.

Unknown aliasing is `may_alias`; `no_alias` always requires an explicit proof.
Indirect slots and reference cells are distinct concepts. All plans must be
complete before any MIR mutation, and every failed operation is failure-atomic.

Production sources are C11. Public internal headers must also compile as C++20.
Do not change W01-W05 IDs, record layouts, dumps, or verifier meanings.
