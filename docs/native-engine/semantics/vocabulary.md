# Shared native-engine semantic vocabulary

These identifiers are shared by the effect model, MIR, and lowering. Each list
is the `catalog` of [`effect-model.json`](effects/effect-model.json) in order,
and equals the matching catalog macro in `Zend/Native/MIR/zend_mir_effects.h`
(label and bit number). New identifiers require source-backed definitions in
the model and a matching catalog entry.

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

## Ownership states

`uninitialized`, `borrowed`, `owned`, `shared_owned`, `moved`, `released`,
and `destroyed`.

## Ownership actions

`borrow`, `copy_addref`, `move`, `produce_owned`, `produce_borrowed`,
`transfer`, `destroy`, `conditional_destroy`, `cow_separate`, and
`canonicalize`.

## Predicates

`may_run_destructor`, `may_bailout`, `may_reenter_php`, `may_invoke_magic`,
`may_observe_frame`, `may_interrupt`, and `may_suspend`.

## Barriers

`safepoint`, `reentrancy`, `destructor`, `exception`, `bailout`, `observer`,
`interrupt`, and `suspend`.

## Guard facts

`value_type`, `object_class_identity`, `refcount_unique`,
`array_packed_layout`, `array_key_presence`, `reference_binding`,
`runtime_cache_entry`, `function_target`, `class_table_entry`,
`no_exception_pending`, `frame_slot_value`, and `destructor_absence`.

## Composition rules

`destructor_closure`, `bailout_is_not_return`, `php_call_closure`,
`unmodeled_internal_call`, `magic_handler_closure`,
`observer_materialization`, `interrupt_materialization`,
`suspension_materialization`, `alias_write_invalidation`,
`exception_cleanup_order`, and `cow_is_visible`.

The effect model defines the observable meaning and conservative behavior of
these identifiers.
