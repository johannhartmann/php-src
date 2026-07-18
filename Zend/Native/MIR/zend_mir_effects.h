/* ZNMIR effect and ownership catalog contract. */

#ifndef ZEND_MIR_EFFECTS_H
#define ZEND_MIR_EFFECTS_H

#include <stdint.h>

#include "zend_mir_ids.h"

#define ZEND_MIR_EFFECT_CATALOG(X) \
	X(READ_MEMORY, "read_memory", 0) \
	X(WRITE_MEMORY, "write_memory", 1) \
	X(ALLOCATE, "allocate", 2) \
	X(THROW, "throw", 3) \
	X(BAILOUT, "bailout", 4) \
	X(CALL_INTERNAL, "call_internal", 5) \
	X(CALL_PHP, "call_php", 6) \
	X(REENTER_PHP, "reenter_php", 7) \
	X(RUN_DESTRUCTOR, "run_destructor", 8) \
	X(OBSERVE_FRAME, "observe_frame", 9) \
	X(INTERRUPT_BOUNDARY, "interrupt_boundary", 10) \
	X(SUSPEND, "suspend", 11) \
	X(RESUME, "resume", 12) \
	X(EXTERNAL_IO, "external_io", 13) \
	X(TERMINATE, "terminate", 14)

#define ZEND_MIR_EFFECT_ENUM(symbol, label, value) ZEND_MIR_EFFECT_##symbol = value,
typedef enum _zend_mir_effect {
	ZEND_MIR_EFFECT_CATALOG(ZEND_MIR_EFFECT_ENUM)
	ZEND_MIR_EFFECT_COUNT = 15,
	ZEND_MIR_EFFECT_INVALID = -1
} zend_mir_effect;
#undef ZEND_MIR_EFFECT_ENUM

#define ZEND_MIR_MEMORY_DOMAIN_CATALOG(X) \
	X(FRAME_ARGS, "frame.args", 0) \
	X(FRAME_LOCALS, "frame.locals", 1) \
	X(FRAME_TEMPS, "frame.temps", 2) \
	X(FRAME_CALL_CHAIN, "frame.call_chain", 3) \
	X(RUNTIME_SYMBOL_TABLE, "runtime.symbol_table", 4) \
	X(RUNTIME_CACHE, "runtime.cache", 5) \
	X(HEAP_ZVAL, "heap.zval", 6) \
	X(HEAP_ARRAY, "heap.array", 7) \
	X(HEAP_OBJECT, "heap.object", 8) \
	X(HEAP_STRING, "heap.string", 9) \
	X(HEAP_REFERENCE, "heap.reference", 10) \
	X(GC_METADATA, "gc.metadata", 11) \
	X(ENGINE_EXCEPTION, "engine.exception", 12) \
	X(ENGINE_OBSERVER, "engine.observer", 13) \
	X(ENGINE_INTERRUPT, "engine.interrupt", 14) \
	X(ENGINE_CLASS_TABLE, "engine.class_table", 15) \
	X(ENGINE_FUNCTION_TABLE, "engine.function_table", 16) \
	X(ENGINE_GENERATOR, "engine.generator", 17) \
	X(ENGINE_FIBER, "engine.fiber", 18) \
	X(EXTERNAL_STATE, "external.state", 19)

#define ZEND_MIR_MEMORY_DOMAIN_ENUM(symbol, label, value) ZEND_MIR_MEMORY_DOMAIN_##symbol = value,
typedef enum _zend_mir_memory_domain {
	ZEND_MIR_MEMORY_DOMAIN_CATALOG(ZEND_MIR_MEMORY_DOMAIN_ENUM)
	ZEND_MIR_MEMORY_DOMAIN_COUNT = 20,
	ZEND_MIR_MEMORY_DOMAIN_INVALID = -1
} zend_mir_memory_domain;
#undef ZEND_MIR_MEMORY_DOMAIN_ENUM

#define ZEND_MIR_OWNERSHIP_STATE_CATALOG(X) \
	X(UNINITIALIZED, "uninitialized", 0) \
	X(BORROWED, "borrowed", 1) \
	X(OWNED, "owned", 2) \
	X(SHARED_OWNED, "shared_owned", 3) \
	X(MOVED, "moved", 4) \
	X(RELEASED, "released", 5) \
	X(DESTROYED, "destroyed", 6)

#define ZEND_MIR_OWNERSHIP_STATE_ENUM(symbol, label, value) ZEND_MIR_OWNERSHIP_STATE_##symbol = value,
typedef enum _zend_mir_ownership_state {
	ZEND_MIR_OWNERSHIP_STATE_CATALOG(ZEND_MIR_OWNERSHIP_STATE_ENUM)
	ZEND_MIR_OWNERSHIP_STATE_COUNT = 7,
	ZEND_MIR_OWNERSHIP_STATE_INVALID = -1
} zend_mir_ownership_state;
#undef ZEND_MIR_OWNERSHIP_STATE_ENUM

#define ZEND_MIR_OWNERSHIP_ACTION_CATALOG(X) \
	X(BORROW, "borrow", 0) \
	X(COPY_ADDREF, "copy_addref", 1) \
	X(MOVE, "move", 2) \
	X(PRODUCE_OWNED, "produce_owned", 3) \
	X(PRODUCE_BORROWED, "produce_borrowed", 4) \
	X(TRANSFER, "transfer", 5) \
	X(DESTROY, "destroy", 6) \
	X(CONDITIONAL_DESTROY, "conditional_destroy", 7) \
	X(COW_SEPARATE, "cow_separate", 8) \
	X(CANONICALIZE, "canonicalize", 9)

#define ZEND_MIR_OWNERSHIP_ACTION_ENUM(symbol, label, value) ZEND_MIR_OWNERSHIP_ACTION_##symbol = value,
typedef enum _zend_mir_ownership_action {
	ZEND_MIR_OWNERSHIP_ACTION_CATALOG(ZEND_MIR_OWNERSHIP_ACTION_ENUM)
	ZEND_MIR_OWNERSHIP_ACTION_COUNT = 10,
	ZEND_MIR_OWNERSHIP_ACTION_INVALID = -1
} zend_mir_ownership_action;
#undef ZEND_MIR_OWNERSHIP_ACTION_ENUM

#define ZEND_MIR_BARRIER_CATALOG(X) \
	X(SAFEPOINT, "safepoint", 0) \
	X(REENTRANCY, "reentrancy", 1) \
	X(DESTRUCTOR, "destructor", 2) \
	X(EXCEPTION, "exception", 3) \
	X(BAILOUT, "bailout", 4) \
	X(OBSERVER, "observer", 5) \
	X(INTERRUPT, "interrupt", 6) \
	X(SUSPEND, "suspend", 7)

#define ZEND_MIR_BARRIER_ENUM(symbol, label, value) ZEND_MIR_BARRIER_##symbol = value,
typedef enum _zend_mir_barrier {
	ZEND_MIR_BARRIER_CATALOG(ZEND_MIR_BARRIER_ENUM)
	ZEND_MIR_BARRIER_COUNT = 8,
	ZEND_MIR_BARRIER_INVALID = -1
} zend_mir_barrier;
#undef ZEND_MIR_BARRIER_ENUM

#define ZEND_MIR_PREDICATE_CATALOG(X) \
	X(MAY_RUN_DESTRUCTOR, "may_run_destructor", 0) \
	X(MAY_BAILOUT, "may_bailout", 1) \
	X(MAY_REENTER_PHP, "may_reenter_php", 2) \
	X(MAY_INVOKE_MAGIC, "may_invoke_magic", 3) \
	X(MAY_OBSERVE_FRAME, "may_observe_frame", 4) \
	X(MAY_INTERRUPT, "may_interrupt", 5) \
	X(MAY_SUSPEND, "may_suspend", 6)

#define ZEND_MIR_PREDICATE_ENUM(symbol, label, value) ZEND_MIR_PREDICATE_##symbol = value,
typedef enum _zend_mir_predicate {
	ZEND_MIR_PREDICATE_CATALOG(ZEND_MIR_PREDICATE_ENUM)
	ZEND_MIR_PREDICATE_COUNT = 7,
	ZEND_MIR_PREDICATE_INVALID = -1
} zend_mir_predicate;
#undef ZEND_MIR_PREDICATE_ENUM

#define ZEND_MIR_GUARD_FACT_CATALOG(X) \
	X(VALUE_TYPE, "value_type", 0) \
	X(OBJECT_CLASS_IDENTITY, "object_class_identity", 1) \
	X(REFCOUNT_UNIQUE, "refcount_unique", 2) \
	X(ARRAY_PACKED_LAYOUT, "array_packed_layout", 3) \
	X(ARRAY_KEY_PRESENCE, "array_key_presence", 4) \
	X(REFERENCE_BINDING, "reference_binding", 5) \
	X(RUNTIME_CACHE_ENTRY, "runtime_cache_entry", 6) \
	X(FUNCTION_TARGET, "function_target", 7) \
	X(CLASS_TABLE_ENTRY, "class_table_entry", 8) \
	X(NO_EXCEPTION_PENDING, "no_exception_pending", 9) \
	X(FRAME_SLOT_VALUE, "frame_slot_value", 10) \
	X(DESTRUCTOR_ABSENCE, "destructor_absence", 11)

#define ZEND_MIR_GUARD_FACT_ENUM(symbol, label, value) ZEND_MIR_GUARD_FACT_##symbol = value,
typedef enum _zend_mir_guard_fact {
	ZEND_MIR_GUARD_FACT_CATALOG(ZEND_MIR_GUARD_FACT_ENUM)
	ZEND_MIR_GUARD_FACT_COUNT = 12,
	ZEND_MIR_GUARD_FACT_INVALID = -1
} zend_mir_guard_fact;
#undef ZEND_MIR_GUARD_FACT_ENUM

#define ZEND_MIR_COMPOSITION_RULE_CATALOG(X) \
	X(DESTRUCTOR_CLOSURE, "destructor_closure", 0) \
	X(BAILOUT_IS_NOT_RETURN, "bailout_is_not_return", 1) \
	X(PHP_CALL_CLOSURE, "php_call_closure", 2) \
	X(UNMODELED_INTERNAL_CALL, "unmodeled_internal_call", 3) \
	X(MAGIC_HANDLER_CLOSURE, "magic_handler_closure", 4) \
	X(OBSERVER_MATERIALIZATION, "observer_materialization", 5) \
	X(INTERRUPT_MATERIALIZATION, "interrupt_materialization", 6) \
	X(SUSPENSION_MATERIALIZATION, "suspension_materialization", 7) \
	X(ALIAS_WRITE_INVALIDATION, "alias_write_invalidation", 8) \
	X(EXCEPTION_CLEANUP_ORDER, "exception_cleanup_order", 9) \
	X(COW_IS_VISIBLE, "cow_is_visible", 10)

#define ZEND_MIR_COMPOSITION_RULE_ENUM(symbol, label, value) ZEND_MIR_COMPOSITION_RULE_##symbol = value,
typedef enum _zend_mir_composition_rule {
	ZEND_MIR_COMPOSITION_RULE_CATALOG(ZEND_MIR_COMPOSITION_RULE_ENUM)
	ZEND_MIR_COMPOSITION_RULE_COUNT = 11,
	ZEND_MIR_COMPOSITION_RULE_INVALID = -1
} zend_mir_composition_rule;
#undef ZEND_MIR_COMPOSITION_RULE_ENUM

typedef uint16_t zend_mir_effect_mask;
typedef uint32_t zend_mir_memory_domain_mask;
typedef uint16_t zend_mir_ownership_action_mask;
typedef uint8_t zend_mir_barrier_mask;
typedef uint8_t zend_mir_predicate_mask;
typedef uint16_t zend_mir_guard_fact_mask;
typedef uint16_t zend_mir_composition_rule_mask;

#define ZEND_MIR_EFFECT_MASK(effect) ((zend_mir_effect_mask) (UINT16_C(1) << (effect)))
#define ZEND_MIR_MEMORY_DOMAIN_MASK(domain) ((zend_mir_memory_domain_mask) (UINT32_C(1) << (domain)))
#define ZEND_MIR_OWNERSHIP_ACTION_MASK(action) ((zend_mir_ownership_action_mask) (UINT16_C(1) << (action)))
#define ZEND_MIR_BARRIER_MASK(barrier) ((zend_mir_barrier_mask) (UINT8_C(1) << (barrier)))
#define ZEND_MIR_PREDICATE_MASK(predicate) ((zend_mir_predicate_mask) (UINT8_C(1) << (predicate)))
#define ZEND_MIR_GUARD_FACT_MASK(fact) ((zend_mir_guard_fact_mask) (UINT16_C(1) << (fact)))
#define ZEND_MIR_COMPOSITION_RULE_MASK(rule) ((zend_mir_composition_rule_mask) (UINT16_C(1) << (rule)))

ZEND_MIR_STATIC_ASSERT(ZEND_MIR_EFFECT_COUNT <= 16, "effect mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_MEMORY_DOMAIN_COUNT <= 32, "domain mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OWNERSHIP_ACTION_COUNT <= 16, "action mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_BARRIER_COUNT <= 8, "barrier mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_PREDICATE_COUNT <= 8, "predicate mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_GUARD_FACT_COUNT <= 16, "guard-fact mask contains the complete W01 catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_COMPOSITION_RULE_COUNT <= 16,
	"composition-rule mask contains the complete W01 catalog");

#endif /* ZEND_MIR_EFFECTS_H */
