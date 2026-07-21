#ifndef ZEND_MIR_VALUES_H
#define ZEND_MIR_VALUES_H

#include <stdbool.h>
#include <stdint.h>

#include "zend_mir.h"

typedef enum _zend_mir_storage_kind {
	ZEND_MIR_STORAGE_FRAME_SLOT = 0,
	ZEND_MIR_STORAGE_TEMPORARY = 1,
	ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT = 2,
	ZEND_MIR_STORAGE_INDIRECT_SLOT = 3,
	ZEND_MIR_STORAGE_CALL_ARGUMENT_SLOT = 4,
	ZEND_MIR_STORAGE_CALL_RETURN_SLOT = 5,
	ZEND_MIR_STORAGE_KIND_INVALID = -1
} zend_mir_storage_kind;

typedef enum _zend_mir_storage_state {
	ZEND_MIR_STORAGE_UNDEF = 0,
	ZEND_MIR_STORAGE_DIRECT = 1,
	ZEND_MIR_STORAGE_REFERENCE = 2,
	ZEND_MIR_STORAGE_INDIRECT = 3,
	ZEND_MIR_STORAGE_STATE_INVALID = -1
} zend_mir_storage_state;

typedef enum _zend_mir_value_category {
	ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR = 0,
	ZEND_MIR_VALUE_REFCOUNTED_STRING = 1,
	ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT = 2,
	ZEND_MIR_VALUE_OBJECT_ABSTRACT = 3,
	ZEND_MIR_VALUE_RESOURCE_ABSTRACT = 4,
	ZEND_MIR_VALUE_REFERENCE_CELL = 5,
	ZEND_MIR_VALUE_CATEGORY_UNKNOWN = 6,
	ZEND_MIR_VALUE_CATEGORY_INVALID = -1
} zend_mir_value_category;

typedef enum _zend_mir_refcount_state {
	ZEND_MIR_REFCOUNT_IMMORTAL = 0,
	ZEND_MIR_REFCOUNT_UNIQUE = 1,
	ZEND_MIR_REFCOUNT_SHARED = 2,
	ZEND_MIR_REFCOUNT_UNKNOWN = 3,
	ZEND_MIR_REFCOUNT_STATE_INVALID = -1
} zend_mir_refcount_state;

typedef enum _zend_mir_transfer_action {
	ZEND_MIR_TRANSFER_BORROW = 0,
	ZEND_MIR_TRANSFER_COPY_ADDREF = 1,
	ZEND_MIR_TRANSFER_MOVE = 2,
	ZEND_MIR_TRANSFER_RELEASE = 3,
	ZEND_MIR_TRANSFER_TO_CALLEE = 4,
	ZEND_MIR_TRANSFER_FROM_CALLEE = 5,
	ZEND_MIR_TRANSFER_ACTION_INVALID = -1
} zend_mir_transfer_action;

typedef enum _zend_mir_alias_relation {
	ZEND_MIR_ALIAS_MUST = 0,
	ZEND_MIR_ALIAS_MAY = 1,
	ZEND_MIR_ALIAS_NONE = 2,
	ZEND_MIR_ALIAS_RELATION_INVALID = -1
} zend_mir_alias_relation;

typedef enum _zend_mir_separation_reason {
	ZEND_MIR_SEPARATION_EXPLICIT = 0,
	ZEND_MIR_SEPARATION_WRITE = 1,
	ZEND_MIR_SEPARATION_CALL_BOUNDARY = 2,
	ZEND_MIR_SEPARATION_REASON_INVALID = -1
} zend_mir_separation_reason;

typedef enum _zend_mir_separation_requirement {
	ZEND_MIR_SEPARATION_REQUIRED_NO = 0,
	ZEND_MIR_SEPARATION_REQUIRED_YES = 1,
	ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN = 2,
	ZEND_MIR_SEPARATION_REQUIREMENT_INVALID = -1
} zend_mir_separation_requirement;

typedef enum _zend_mir_parameter_mode {
	ZEND_MIR_PARAMETER_BY_VALUE = 0,
	ZEND_MIR_PARAMETER_BY_REFERENCE = 1,
	ZEND_MIR_PARAMETER_MODE_INVALID = -1
} zend_mir_parameter_mode;

typedef struct _zend_mir_storage_ref {
	zend_mir_storage_id id;
	zend_mir_storage_kind kind;
	zend_mir_storage_state state;
	zend_mir_value_category category;
	zend_mir_payload_id payload_id;
	zend_mir_reference_cell_id reference_cell_id;
	zend_mir_storage_id indirect_target_id;
} zend_mir_storage_ref;

typedef struct _zend_mir_payload_ref {
	zend_mir_payload_id id;
	zend_mir_value_category category;
	zend_mir_refcount_state refcount_state;
	bool cleanup_obligation;
} zend_mir_payload_ref;

typedef struct _zend_mir_reference_cell_ref {
	zend_mir_reference_cell_id id;
	zend_mir_storage_id payload_storage_id;
	zend_mir_alias_class_id alias_class_id;
	zend_mir_source_position_id creation_source_id;
	zend_mir_ownership_state ownership;
	bool cleanup_obligation;
} zend_mir_reference_cell_ref;

typedef struct _zend_mir_alias_relation_ref {
	zend_mir_alias_class_id left_id;
	zend_mir_alias_class_id right_id;
	zend_mir_alias_relation relation;
	uint32_t proof_id;
} zend_mir_alias_relation_ref;

typedef struct _zend_mir_ownership_event_ref {
	zend_mir_ownership_event_id id;
	zend_mir_storage_id source_storage_id;
	zend_mir_storage_id target_storage_id;
	zend_mir_payload_id payload_id;
	zend_mir_transfer_action action;
	zend_mir_refcount_state before_state;
	zend_mir_refcount_state after_state;
	bool cleanup_obligation;
} zend_mir_ownership_event_ref;

typedef struct _zend_mir_separation_plan_ref {
	zend_mir_separation_plan_id id;
	zend_mir_payload_id source_payload_id;
	zend_mir_storage_id source_storage_id;
	zend_mir_separation_reason reason;
	zend_mir_refcount_state uniqueness_fact;
	zend_mir_separation_requirement required;
	zend_mir_payload_id result_payload_id;
	bool clone_execution_required;
} zend_mir_separation_plan_ref;

typedef struct _zend_mir_parameter_mode_ref {
	zend_mir_parameter_mode_id id;
	uint32_t ordinal;
	zend_mir_parameter_mode mode;
} zend_mir_parameter_mode_ref;

typedef struct _zend_mir_call_transfer_ref {
	zend_mir_call_site_id call_site_id;
	zend_mir_span parameter_modes;
	uint32_t argument_ordinal;
	zend_mir_storage_id argument_storage_id;
	zend_mir_reference_cell_id argument_reference_cell_id;
	zend_mir_transfer_action argument_action;
	zend_mir_storage_id return_storage_id;
	zend_mir_reference_cell_id return_reference_cell_id;
	zend_mir_transfer_action return_action;
} zend_mir_call_transfer_ref;

typedef struct _zend_mir_value_view {
	uint32_t contract_version;
	const void *context;
	uint32_t (*storage_count)(const void *context);
	bool (*storage_at)(const void *context, uint32_t index, zend_mir_storage_ref *out);
	uint32_t (*payload_count)(const void *context);
	bool (*payload_at)(const void *context, uint32_t index, zend_mir_payload_ref *out);
	uint32_t (*reference_cell_count)(const void *context);
	bool (*reference_cell_at)(const void *context, uint32_t index,
		zend_mir_reference_cell_ref *out);
	uint32_t (*alias_relation_count)(const void *context);
	bool (*alias_relation_at)(const void *context, uint32_t index,
		zend_mir_alias_relation_ref *out);
	uint32_t (*ownership_event_count)(const void *context);
	bool (*ownership_event_at)(const void *context, uint32_t index,
		zend_mir_ownership_event_ref *out);
	uint32_t (*separation_plan_count)(const void *context);
	bool (*separation_plan_at)(const void *context, uint32_t index,
		zend_mir_separation_plan_ref *out);
	uint32_t (*call_transfer_count)(const void *context);
	bool (*call_transfer_at)(const void *context, uint32_t index,
		zend_mir_call_transfer_ref *out);
} zend_mir_value_view;

typedef struct _zend_mir_value_mutator {
	uint32_t contract_version;
	void *context;
	bool (*add_storage)(void *context, const zend_mir_storage_ref *record);
	bool (*add_payload)(void *context, const zend_mir_payload_ref *record);
	bool (*add_reference_cell)(void *context, const zend_mir_reference_cell_ref *record);
	bool (*add_alias_relation)(void *context, const zend_mir_alias_relation_ref *record);
	bool (*add_ownership_event)(void *context, const zend_mir_ownership_event_ref *record);
	bool (*add_separation_plan)(void *context, const zend_mir_separation_plan_ref *record);
	bool (*add_call_transfer)(void *context, const zend_mir_call_transfer_ref *record);
} zend_mir_value_mutator;

typedef enum _zend_mir_verify_w06_code {
	ZEND_MIR_VERIFY_W06_OK = 0,
	ZEND_MIR_VERIFY_W06_STORAGE_MISMATCH = 800,
	ZEND_MIR_VERIFY_W06_REFERENCE_MISMATCH = 801,
	ZEND_MIR_VERIFY_W06_INDIRECT_MISMATCH = 802,
	ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH = 803,
	ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH = 804,
	ZEND_MIR_VERIFY_W06_SEPARATION_MISMATCH = 805,
	ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH = 806,
	ZEND_MIR_VERIFY_W06_CODE_INVALID = -1
} zend_mir_verify_w06_code;

#define ZEND_MIRV_TOKEN_W06_STORAGE_MISMATCH "[MIRV0800]"
#define ZEND_MIRV_TOKEN_W06_REFERENCE_MISMATCH "[MIRV0801]"
#define ZEND_MIRV_TOKEN_W06_INDIRECT_MISMATCH "[MIRV0802]"
#define ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH "[MIRV0803]"
#define ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH "[MIRV0804]"
#define ZEND_MIRV_TOKEN_W06_SEPARATION_MISMATCH "[MIRV0805]"
#define ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH "[MIRV0806]"

bool zend_mir_verify_w06_values(const zend_mir_view *view,
	const zend_mir_value_view *values, zend_mir_diagnostic_sink *diagnostics);

#endif /* ZEND_MIR_VALUES_H */
