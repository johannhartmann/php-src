#ifndef ZEND_MIR_VALUE_SOURCE_H
#define ZEND_MIR_VALUE_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../Lowering/zend_mir_lowering_source.h"
#include "../../MIR/zend_mir_values.h"

typedef enum _zend_mir_source_value_provenance {
	ZEND_MIR_SOURCE_VALUE_LOCAL = 0,
	ZEND_MIR_SOURCE_VALUE_CALL_ARGUMENT = 1,
	ZEND_MIR_SOURCE_VALUE_CALL_RESULT = 2,
	ZEND_MIR_SOURCE_VALUE_GLOBAL_OR_DYNAMIC = 3,
	ZEND_MIR_SOURCE_VALUE_PROVENANCE_INVALID = -1
} zend_mir_source_value_provenance;

typedef struct _zend_mir_source_storage_ref {
	zend_mir_storage_id id;
	zend_mir_source_slot_kind slot_kind;
	uint32_t slot_index;
	uint32_t ssa_variable_id;
	zend_mir_storage_kind kind;
	zend_mir_storage_state state;
	zend_mir_value_category category;
	zend_mir_source_value_provenance provenance;
} zend_mir_source_storage_ref;

typedef struct _zend_mir_source_reference_ref {
	zend_mir_reference_cell_id id;
	zend_mir_storage_id payload_storage_id;
	zend_mir_alias_class_id alias_class_id;
	uint32_t creation_opline_index;
	zend_mir_ownership_state ownership;
	bool cleanup_obligation;
} zend_mir_source_reference_ref;

typedef struct _zend_mir_source_indirect_ref {
	zend_mir_storage_id indirect_storage_id;
	zend_mir_storage_id target_storage_id;
	uint32_t source_opline_index;
} zend_mir_source_indirect_ref;

/*
 * Source tables are complete immutable snapshots built before mutation.
 * Storage order is stable source-slot order; reference and indirect tables are
 * ordered by their stable IDs. No callback returns a Zend pointer.
 */
typedef struct _zend_mir_source_value_view {
	uint32_t contract_version;
	const void *context;
	uint32_t (*storage_count)(const void *context);
	bool (*storage_at)(const void *context, uint32_t index,
		zend_mir_source_storage_ref *out);
	uint32_t (*reference_count)(const void *context);
	bool (*reference_at)(const void *context, uint32_t index,
		zend_mir_source_reference_ref *out);
	uint32_t (*indirect_count)(const void *context);
	bool (*indirect_at)(const void *context, uint32_t index,
		zend_mir_source_indirect_ref *out);
	uint32_t (*opcode_count)(const void *context);
	bool (*opcode_at)(const void *context, uint32_t index,
		zend_mir_source_opcode_ref *out);
} zend_mir_source_value_view;

#endif /* ZEND_MIR_VALUE_SOURCE_H */
