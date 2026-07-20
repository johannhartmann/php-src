#ifndef ZEND_MIR_CAPABILITY_H
#define ZEND_MIR_CAPABILITY_H

#include <stdint.h>

#include "zend_mir.h"

/*
 * Stable transport IDs are defined by capability-registry.json. Persistent
 * modules store sorted spans of these IDs. The W05 bit masks remain a derived
 * compatibility view and are never the source of truth.
 */
typedef uint32_t zend_mir_capability_id;
typedef uint32_t zend_mir_semantic_debt_id;

enum {
	ZEND_MIR_CAP_ARCHITECTURE_INDEPENDENT_MIR = 1,
	ZEND_MIR_CAP_SCALAR_SEMANTICS = 2,
	ZEND_MIR_CAP_REDUCIBLE_CONTROL_FLOW = 3,
	ZEND_MIR_CAP_DIRECT_USER_CALL_SEQUENCE = 4,
	ZEND_MIR_CAP_CALLER_FRAME_DESCRIPTOR = 5,
	ZEND_MIR_CAP_CALLEE_ENTRY_DESCRIPTOR = 6,
	ZEND_MIR_CAP_ABSTRACT_CALL_EFFECTS = 7
};

enum {
	ZEND_MIR_DEBT_CALL_EXECUTION = 1001,
	ZEND_MIR_DEBT_EXCEPTION_CLEANUP = 1002,
	ZEND_MIR_DEBT_REFCOUNTED_TRANSFER = 1003,
	ZEND_MIR_DEBT_PROTECTED_CONTINUATION = 1004,
	ZEND_MIR_DEBT_DYNAMIC_TARGET_RESOLUTION = 1005,
	ZEND_MIR_DEBT_OBSERVER_INTEROP = 1006,
	ZEND_MIR_DEBT_COW_INDIRECT_SEMANTICS = 1007,
	ZEND_MIR_DEBT_INTERNAL_C_ABI_INTEROP = 1008
};

typedef struct _zend_mir_capability_set_ref {
	zend_mir_span capability_ids;
	zend_mir_span semantic_debt_ids;
	bool codegen_eligible;
} zend_mir_capability_set_ref;

#endif /* ZEND_MIR_CAPABILITY_H */
