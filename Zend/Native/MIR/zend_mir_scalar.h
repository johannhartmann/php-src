/* Target-neutral scalar facts consumed by W03 lowering. */

#ifndef ZEND_MIR_SCALAR_H
#define ZEND_MIR_SCALAR_H

#include <stdint.h>

#include "zend_mir_ids.h"

typedef uint32_t zend_mir_scalar_type_mask;
typedef zend_mir_scalar_type_mask zend_mir_php_scalar_type_mask;

enum {
	ZEND_MIR_SCALAR_TYPE_NONE = 0,
	ZEND_MIR_SCALAR_TYPE_NULL = UINT32_C(1) << 0,
	ZEND_MIR_SCALAR_TYPE_I1 = UINT32_C(1) << 1,
	ZEND_MIR_SCALAR_TYPE_I64 = UINT32_C(1) << 2,
	ZEND_MIR_SCALAR_TYPE_F64 = UINT32_C(1) << 3,
	ZEND_MIR_SCALAR_TYPE_ALL = ZEND_MIR_SCALAR_TYPE_NULL
		| ZEND_MIR_SCALAR_TYPE_I1
		| ZEND_MIR_SCALAR_TYPE_I64
		| ZEND_MIR_SCALAR_TYPE_F64
};

typedef enum _zend_mir_fact_provenance {
	ZEND_MIR_FACT_PROVENANCE_SSA = 0,
	ZEND_MIR_FACT_PROVENANCE_LITERAL = 1,
	ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS = 2,
	ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS = 3,
	ZEND_MIR_FACT_PROVENANCE_CONTRACT = 4,
	ZEND_MIR_FACT_PROVENANCE_COUNT = 5,
	ZEND_MIR_FACT_PROVENANCE_INVALID = -1
} zend_mir_fact_provenance;

typedef uint32_t zend_mir_value_fact_flags;

enum {
	ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE = UINT32_C(1) << 0,
	ZEND_MIR_VALUE_FACT_NONZERO = UINT32_C(1) << 1,
	ZEND_MIR_VALUE_FACT_FINITE = UINT32_C(1) << 2,
	ZEND_MIR_VALUE_FACT_NON_REFCOUNTED = UINT32_C(1) << 3
};

/*
 * Immutable value-fact snapshot. exact_type must contain exactly one scalar
 * type bit. Range fields are meaningful only with HAS_INTEGER_RANGE.
 */
typedef struct _zend_mir_value_fact_ref {
	zend_mir_value_fact_id id;
	zend_mir_value_id value_id;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_value_fact_flags flags;
	int64_t integer_min;
	int64_t integer_max;
	zend_mir_fact_provenance provenance;
	zend_mir_source_position_id provenance_source_position_id;
} zend_mir_value_fact_ref;

static inline bool zend_mir_scalar_type_is_exact(zend_mir_scalar_type_mask type)
{
	return type != ZEND_MIR_SCALAR_TYPE_NONE
		&& (type & ~ZEND_MIR_SCALAR_TYPE_ALL) == 0
		&& (type & (type - UINT32_C(1))) == 0;
}

#endif /* ZEND_MIR_SCALAR_H */
