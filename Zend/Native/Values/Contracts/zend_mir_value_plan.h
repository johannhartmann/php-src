#ifndef ZEND_MIR_VALUE_PLAN_H
#define ZEND_MIR_VALUE_PLAN_H

#include "zend_mir_value_source.h"

typedef enum _zend_mir_value_plan_decision {
	ZEND_MIR_VALUE_PLAN_ACCEPTED = 0,
	ZEND_MIR_VALUE_PLAN_DEFERRED = 1,
	ZEND_MIR_VALUE_PLAN_REJECTED = 2,
	ZEND_MIR_VALUE_PLAN_DECISION_INVALID = -1
} zend_mir_value_plan_decision;

typedef struct _zend_mir_value_plan_entry {
	uint32_t source_opline_index;
	zend_mir_value_plan_decision decision;
	uint32_t diagnostic_code;
	zend_mir_span storage_span;
	zend_mir_span ownership_event_span;
	zend_mir_span separation_plan_span;
} zend_mir_value_plan_entry;

/*
 * Planning inventories all reachable value state before mutation. A failed
 * allocation or invalid record publishes no entries. Unsupported semantics
 * may produce immutable deferred entries for diagnostics, but no MIR writes.
 */
typedef struct _zend_mir_value_plan {
	const zend_mir_value_plan_entry *entries;
	uint32_t count;
	bool complete;
	bool immutable;
} zend_mir_value_plan;

#endif /* ZEND_MIR_VALUE_PLAN_H */
