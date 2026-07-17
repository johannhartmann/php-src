# Code-motion examples

These are semantic pseudocode rewrites.  `Before` is the input order and
`After` is the proposed order; an allowed rewrite requires every stated proof.

## Allowed with proof

1. **Before:** `integer_add; type_guard($x)`. **After:** `type_guard($x);
   integer_add`. **Proof:** `$x` is SSA-local, the add is total, and no alias is
   written.
2. **Before:** `type_guard($x); scalar_read; type_guard($x)`. **After:**
   `type_guard($x); scalar_read`. **Proof:** the read has no invalidator.
3. **Before:** loop `{ string_length($owned); body }`. **After:**
   `n = string_length($owned); loop { n; body }`. **Proof:** the string has no
   writable aliases or escape.
4. **Before:** `packed_guard($a); element_read($a); packed_guard($a)`. **After:**
   `packed_guard($a); element_read($a)`. **Proof:** the read cannot expose a
   reference or invoke user behavior.
5. **Before:** `read($local_a); read($local_b)`. **After:** `read($local_b);
   read($local_a)`. **Proof:** the scalar slots are disjoint and neither read
   can throw or observe the frame.
6. **Before:** `copy_addref($scalar); total_work; use($copy)`. **After:**
   `total_work; copy_addref($scalar); use($copy)`. **Proof:** no exceptional,
   destructor, observer, interrupt, or alias edge intervenes.
7. **Before:** `copy_addref($owned); move($copy)`. **After:** `move($owned)`.
   **Proof:** the source has one use and exactly one cleanup obligation reaches
   every exit.
8. **Before:** `cow_separate($a); read_only_work; array_write($a)`. **After:**
   `read_only_work; cow_separate($a); array_write($a)`. **Proof:** no alias,
   uniqueness, refcount, layout, or barrier fact changes in `read_only_work`.
9. **Before:** `binding_guard($r); scalar_read; binding_guard($r)`. **After:**
   `binding_guard($r); scalar_read`. **Proof:** no reference binding or pointee
   alias is written.
10. **Before:** `cache_lookup($slot); local_read; cache_lookup($slot)`. **After:**
    `cache_lookup($slot); local_read`. **Proof:** no call, reentry, interrupt,
    cache write, or table write intervenes.
11. **Before:** `exception_check; total_scalar_calc`. **After:**
    `total_scalar_calc; exception_check`. **Proof:** the calculation cannot
    throw, allocate, observe state, or create cleanup.
12. **Before:** `class_guard($o); scalar_read; class_guard($o)`. **After:**
    `class_guard($o); scalar_read`. **Proof:** exact handlers exclude magic
    access and the read cannot write an alias.
13. **Before:** `destroy($scalar_a); destroy($scalar_b)`. **After:**
    `destroy($scalar_b); destroy($scalar_a)`. **Proof:** exact types establish
    that neither cleanup executes user code, throws, triggers GC, or aliases.
14. **Before:** `frame_load($local); total_arithmetic`. **After:**
    `total_arithmetic; frame_load($local)`. **Proof:** the slot has no reference
    or symbol-table alias and arithmetic cannot reach an observer.

## Forbidden

1. **Before:** `cow_separate($a); array_write($a)`. **After:**
   `array_write($a); cow_separate($a)`. **Reason:** separation is a visible
   ownership, refcount, allocation, and memory effect.
2. **Before:** `packed_guard($a); write($may_alias_reference); packed_load($a)`.
   **After:** hoist `packed_load($a)` before the write. **Reason:** the write can
   invalidate packed layout and bounds.
3. **Before:** `destroy($first); destroy($second)`. **After:** reverse the
   destroys. **Reason:** destructor PHP reentry and visible cleanup order change.
4. **Before:** `destroy($object); return`. **After:** `return; destroy($object)`.
   **Reason:** the destructor must run before the caller resumes.
5. **Before:** `property_read($o); call_php()`. **After:** `call_php();
   property_read($o)`. **Reason:** magic `__get`, handlers, mutation, and throw
   behavior can differ.
6. **Before:** `property_write($o); destroy($x)`. **After:** reverse the actions.
   **Reason:** magic `__set`, destructor reentry, and exception order are visible.
7. **Before:** `if bailout -> terminal else success_work`. **After:** execute
   `success_work` on both edges. **Reason:** bailout is not a normal return.
8. **Before:** `destroy($x); may_throw()`. **After:** `may_throw(); destroy($x)`.
   **Reason:** the exception can change whether and when cleanup executes.
9. **Before:** `materialize(frame); observer_callback()`. **After:** defer
   materialization until after the callback. **Reason:** the observer sees frame
   arguments, locals, temporaries, and call position.
10. **Before:** `cache_lookup($slot); call_php(); use_cached_target`. **After:**
    hoist `use_cached_target` before validation after the call. **Reason:**
    runtime caches and class/function tables can change during reentry.
11. **Before:** `unique_guard($x); copy_addref($x); in_place_write($x)`. **After:**
    reuse the uniqueness fact after the copy. **Reason:** the added owner and GC
    metadata write invalidate uniqueness.
12. **Before:** `external_io(); interrupt_check()`. **After:** reverse the
    operations. **Reason:** callback, termination, and external ordering change.
13. **Before:** PHI edges `borrowed($x)` and `owned($x)`. **After:** merge them as
    one owned result. **Reason:** an explicit edge conversion and exactly-once
    cleanup are required.
14. **Before:** `destroy($x); move($x)`. **After:** accept the move as live.
    **Reason:** destroyed, released, and moved ownership states are terminal.
15. **Before:** `generator_load($slot); suspend; use($slot)`. **After:** reuse the
    pre-suspend guard after resume. **Reason:** generator/fiber state must be
    materialized and guards revalidated.
16. **Before:** `unmodeled_internal_call(); optimized_read`. **After:** classify
    the call as pure and hoist the read. **Reason:** the fail-closed summary reads
    and writes all domains and installs all barriers.
