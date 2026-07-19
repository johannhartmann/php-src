#ifndef ZEND_MIR_CALL_PLAN_H
#define ZEND_MIR_CALL_PLAN_H

#include "zend_mir_call_source.h"

typedef enum _zend_mir_call_plan_decision {
	ZEND_MIR_CALL_PLAN_ACCEPTED = 0,
	ZEND_MIR_CALL_PLAN_DEFERRED = 1,
	ZEND_MIR_CALL_PLAN_REJECTED = 2,
	ZEND_MIR_CALL_PLAN_DECISION_INVALID = -1
} zend_mir_call_plan_decision;

typedef struct _zend_mir_call_plan_entry {
	zend_mir_source_call_site_id source_call_site_id;
	zend_mir_call_plan_decision decision;
	uint32_t diagnostic_code;
	zend_mir_span argument_span;
} zend_mir_call_plan_entry;

/*
 * Planning scans the complete reachable opcode sequence before mutation,
 * validates a nested INIT/SEND/DO stack, and publishes all entries or none.
 * A failed allocation, orphan SEND/DO, or mismatched nesting leaves count zero
 * and entries NULL. A semantically complete but unsupported call may publish
 * an immutable DEFERRED entry for diagnostics, but must not cause MIR mutation.
 */
typedef struct _zend_mir_call_plan {
	const zend_mir_call_plan_entry *entries;
	uint32_t count;
	bool complete;
	bool immutable;
} zend_mir_call_plan;

#endif /* ZEND_MIR_CALL_PLAN_H */
