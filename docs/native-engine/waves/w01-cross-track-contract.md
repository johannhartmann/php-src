# W01 cross-track semantic vocabulary

This contract freezes the identifiers shared by the W01 opcode inventory and
effect model. The identifiers are normative inputs to later waves. New
identifiers may be added with source-backed definitions, but an existing
identifier may not be renamed or assigned a different meaning without an
explicit contract migration.

## Atomic effects

`read_memory`, `write_memory`, `allocate`, `throw`, `bailout`,
`call_internal`, `call_php`, `reenter_php`, `run_destructor`, `observe_frame`,
`interrupt_boundary`, `suspend`, `resume`, `external_io`, and `terminate`.

## Memory domains

`frame.args`, `frame.locals`, `frame.temps`, `frame.call_chain`,
`runtime.symbol_table`, `runtime.cache`, `heap.zval`, `heap.array`,
`heap.object`, `heap.string`, `heap.reference`, `gc.metadata`,
`engine.exception`, `engine.observer`, `engine.interrupt`,
`engine.class_table`, `engine.function_table`, `engine.generator`,
`engine.fiber`, and `external.state`.

## Ownership actions

`borrow`, `copy_addref`, `move`, `produce_owned`, `produce_borrowed`,
`transfer`, `destroy`, `conditional_destroy`, `cow_separate`, and
`canonicalize`.

## Barriers

`safepoint`, `reentrancy`, `destructor`, `exception`, `bailout`, `observer`,
`interrupt`, and `suspend`.

The effect model defines the observable meaning and conservative behavior of
these identifiers. The opcode matrix may only reference registered identifiers.
An unresolved mismatch is a W01 gate failure, not an implicit extension.
