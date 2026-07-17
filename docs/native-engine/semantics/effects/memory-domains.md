# Memory domains and aliases

Memory effects name semantic domains rather than addresses.  Frame arguments,
locals, temporaries, and the call chain are separate names so a proof can be
precise, but they are not presumed disjoint.  References, indirect zvals,
symbol-table exposure, magic accessors, and calls may connect them to heap and
runtime state.

The model therefore starts from `may_alias`.  A transformation may use
disjointness only when a dominating, operation-specific proof establishes it.
The declared alias relations are minimum edges, not a closed-world promise.
An unrecognized domain is an invalid model reference; an unmodeled operation
is conservatively treated as reading and writing every registered domain.

## Domain groups

- `frame.*` covers arguments, locals, temporaries, and the observable call
  chain.  Calls, observers, interrupts, exceptions, and suspension require
  materialization of the relevant frame state.
- `runtime.*` and engine tables cover the symbol table, caches, and lookup
  state.  A PHP call or reentry can change these even when the immediate
  opcode appears unrelated.
- `heap.*` covers zvals, arrays, objects, strings, references, and GC metadata.
  Refcount changes and copy-on-write separation are visible writes.
- `engine.*` covers exceptions, observers, interrupts, class/function tables,
  generators, and fibers.
- `external.state` covers state visible outside the engine, including I/O.

Array packed/hash layout, key presence, reference bindings, and refcount
uniqueness are guard facts over these domains.  An aliasing write, call,
destructor, or reentry invalidates the corresponding fact unless a stronger
proof says otherwise.

Source basis includes `zend_ssa.h` alias and escape classifications,
`zend_hash.c` layout-changing updates, and zval/reference operations in
`zend_variables.c` and `zend_execute.h`.
