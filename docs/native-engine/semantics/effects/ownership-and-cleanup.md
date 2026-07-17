# Ownership and cleanup

Ownership is a semantic obligation.  `borrowed` does not own a decrement;
`owned` owns exactly one release; `shared_owned` also owns one release but is
known to share storage; and `moved`, `released`, and `destroyed` are terminal
for that source value.  Terminal values cannot be revived or used by another
ownership action.

Copying a zval is not one operation: `copy_addref` produces a new owner and
performs a refcount-visible write.  `move` transfers the obligation and marks
the source moved.  `cow_separate` is an explicit ownership and memory effect,
not a representation-only optimization.  `canonicalize` may change only
representation and must be paired with an ownership action whenever ownership
changes.

## PHI merges

Each incoming PHI edge must carry the same live ownership state or perform an
explicit edge conversion.  Borrowed and owned values never merge implicitly.
Moved, released, and destroyed values cannot be live PHI inputs.  Exactly one
cleanup obligation must emerge from the merge; a frontend must not duplicate
or drop cleanup when critical edges are split.

## Exceptional exits and cleanup order

Cleanup order is observable because releasing an object or reference may run
a destructor, execute PHP, throw, inspect the frame, or trigger GC.  Normal,
exception, bailout, and suspension exits therefore have distinct edge
obligations.  A bailout is not a normal return and must not execute normal
success continuations.

Before any call, observer callback, interrupt check, destructor, exception
dispatch, bailout, or suspension boundary, live interpreter-visible values
must be materialized.  Cleanup remains in source order unless a proof shows
that no release can run user code, throw, observe the frame, or affect an
alias.  A pending exception also cannot be cleared or replaced merely to make
code motion convenient.

This contract follows the destructor and exception behavior in
`zend_objects.c`, `zend_exceptions.c`, `zend_variables.c`, and the VM exception
and yield handlers in `zend_vm_def.h`.
