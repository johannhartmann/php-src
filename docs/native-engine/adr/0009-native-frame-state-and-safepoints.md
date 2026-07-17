# ADR 0009: Native frame state and safepoints

## Status

Accepted on 2026-07-17.

## Context

ADR 0004 requires Zend-compatible baseline frames at every observable boundary.
That requirement needs one concrete definition of frame contents, ownership,
roots, cleanup, and instruction position. Without it, a backend could retain
live PHP values in private registers while a helper, observer, interrupt,
destructor, exception, or suspension path observes an incomplete frame.

## Decision

For every normal non-inlined PHP call, the canonical native baseline frame is a
`zend_execute_data` call frame followed by Zend VM-stack slots for arguments,
compiled variables, temporaries, extra arguments, and pending calls. The native
engine preserves the existing Zend C ABI; private native metadata cannot
replace or resize public frame structures.

At every safepoint the entire observable state is canonical: function and
`op_array` identity, current opline, parent call chain, arguments, locals,
temporaries that are live at that opline, `$this`, return storage, pending call
state, roots, ownership, and cleanup obligations. A private register may hold an
unobservable scalar or a duplicate cache, but it is never the sole home of a
live PHP value across a safepoint.

The normative baseline layout and lifecycle are in the
[baseline frame contract](../semantics/frames/baseline-frame-contract.md). The
complete safepoint set and precise opline rules are in the
[safepoint contract](../semantics/frames/safepoint-contract.md). The
[frame-state schema](../semantics/frames/frame-state.schema.json) is the
machine-readable representation of the same state.

## Consequences

- Zend helpers, extensions, observers, GC, and reentrant execution see the
  existing call-frame ABI.
- Code generation must spill, materialize, root, and assign cleanup before a
  potentially observing operation.
- Safepoint metadata is verified before an immutable code version is
  published; incomplete metadata makes publication fail.
- Future inlining may describe logical parent frames in metadata, but must
  materialize the same canonical chain before observation.

## Alternatives

- A second native-only call-frame ABI was rejected because Zend-facing code
  already consumes `zend_execute_data` and VM-stack slots.
- Best-effort reconstruction after entering a helper was rejected because the
  helper may allocate, throw, reenter, suspend, or bail out before returning.
- Treating registers as implicit GC roots was rejected because existing Zend
  root discovery does not scan an undocumented native register map.

## Verification impact

The validator and negative tests must reject missing required fields, parent
cycles, duplicate slots, missing roots, and missing cleanup obligations. Review
must account for every safepoint class and verify that no public Zend structure
or C ABI is changed by this contract.
