#ifndef ZEND_MIR_VALUE_LOWERING_INVENTORY_H
#define ZEND_MIR_VALUE_LOWERING_INVENTORY_H

#include "zend_mir_value_source.h"

typedef enum _zend_mir_value_lowering_decision {
	ZEND_MIR_VALUE_LOWERING_ACCEPTED = 0,
	ZEND_MIR_VALUE_LOWERING_DEFERRED = 1,
	ZEND_MIR_VALUE_LOWERING_REJECTED = 2,
	ZEND_MIR_VALUE_LOWERING_DECISION_INVALID = -1
} zend_mir_value_lowering_decision;

typedef struct _zend_mir_value_lowering_inventory_entry {
	uint32_t source_opline_index;
	zend_mir_value_lowering_decision decision;
	uint32_t diagnostic_code;
	zend_mir_span storage_span;
	zend_mir_span ownership_event_span;
	zend_mir_span separation_plan_span;
} zend_mir_value_lowering_inventory_entry;

/*
 * This is the process-local W06 per-opline lowering inventory. It is not the
 * persistent per-value execution plan stored in the MIR value model.
 */
typedef struct _zend_mir_value_lowering_inventory {
	const zend_mir_value_lowering_inventory_entry *entries;
	uint32_t count;
	bool complete;
	bool immutable;
} zend_mir_value_lowering_inventory;

#endif /* ZEND_MIR_VALUE_LOWERING_INVENTORY_H */
