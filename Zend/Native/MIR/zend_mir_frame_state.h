/* Immutable ZNMIR frame-state and source-location contract. */

#ifndef ZEND_MIR_FRAME_STATE_H
#define ZEND_MIR_FRAME_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "zend_mir_ids.h"

#define ZEND_MIR_FUNCTION_KIND_CATALOG(X) \
	X(USER, "user", 0) \
	X(INTERNAL, "internal", 1) \
	X(MAIN, "main", 2)
#define ZEND_MIR_FUNCTION_KIND_ENUM(symbol, label, value) ZEND_MIR_FUNCTION_KIND_##symbol = value,
typedef enum _zend_mir_function_kind {
	ZEND_MIR_FUNCTION_KIND_CATALOG(ZEND_MIR_FUNCTION_KIND_ENUM)
	ZEND_MIR_FUNCTION_KIND_COUNT = 3,
	ZEND_MIR_FUNCTION_KIND_INVALID = -1
} zend_mir_function_kind;
#undef ZEND_MIR_FUNCTION_KIND_ENUM

#define ZEND_MIR_OPLINE_PHASE_CATALOG(X) \
	X(BEFORE, "before", 0) \
	X(AFTER, "after", 1) \
	X(EXCEPTION, "exception", 2) \
	X(SUSPENDED, "suspended", 3)
#define ZEND_MIR_OPLINE_PHASE_ENUM(symbol, label, value) ZEND_MIR_OPLINE_PHASE_##symbol = value,
typedef enum _zend_mir_opline_phase {
	ZEND_MIR_OPLINE_PHASE_CATALOG(ZEND_MIR_OPLINE_PHASE_ENUM)
	ZEND_MIR_OPLINE_PHASE_COUNT = 4,
	ZEND_MIR_OPLINE_PHASE_INVALID = -1
} zend_mir_opline_phase;
#undef ZEND_MIR_OPLINE_PHASE_ENUM

#define ZEND_MIR_SUSPEND_KIND_CATALOG(X) \
	X(NONE, "none", 0) \
	X(GENERATOR, "generator", 1) \
	X(FIBER, "fiber", 2) \
	X(DEOPT, "deopt", 3)
#define ZEND_MIR_SUSPEND_KIND_ENUM(symbol, label, value) ZEND_MIR_SUSPEND_KIND_##symbol = value,
typedef enum _zend_mir_suspend_kind {
	ZEND_MIR_SUSPEND_KIND_CATALOG(ZEND_MIR_SUSPEND_KIND_ENUM)
	ZEND_MIR_SUSPEND_KIND_COUNT = 4,
	ZEND_MIR_SUSPEND_KIND_INVALID = -1
} zend_mir_suspend_kind;
#undef ZEND_MIR_SUSPEND_KIND_ENUM

#define ZEND_MIR_RESUME_ENTRY_KIND_CATALOG(X) \
	X(NONE, "none", 0) \
	X(SINGLE_ENTRY_DISPATCHER, "single_entry_dispatcher", 1)
#define ZEND_MIR_RESUME_ENTRY_KIND_ENUM(symbol, label, value) ZEND_MIR_RESUME_ENTRY_KIND_##symbol = value,
typedef enum _zend_mir_resume_entry_kind {
	ZEND_MIR_RESUME_ENTRY_KIND_CATALOG(ZEND_MIR_RESUME_ENTRY_KIND_ENUM)
	ZEND_MIR_RESUME_ENTRY_KIND_COUNT = 2,
	ZEND_MIR_RESUME_ENTRY_KIND_INVALID = -1
} zend_mir_resume_entry_kind;
#undef ZEND_MIR_RESUME_ENTRY_KIND_ENUM

#define ZEND_MIR_SAFEPOINT_CLASS_CATALOG(X) \
	X(FUNCTION_ENTRY, "function_entry", 0) \
	X(USER_CALL, "user_call", 1) \
	X(INTERNAL_CALL, "internal_call", 2) \
	X(ALLOCATION, "allocation", 3) \
	X(DESTRUCTOR, "destructor", 4) \
	X(EXCEPTION_EDGE, "exception_edge", 5) \
	X(BAILOUT_HELPER, "bailout_helper", 6) \
	X(OBSERVER, "observer", 7) \
	X(INTERRUPT, "interrupt", 8) \
	X(GENERATOR_SUSPEND, "generator_suspend", 9) \
	X(GENERATOR_RESUME, "generator_resume", 10) \
	X(FIBER_SWITCH, "fiber_switch", 11) \
	X(DEOPT_RESUME, "deopt_resume", 12)
#define ZEND_MIR_SAFEPOINT_CLASS_ENUM(symbol, label, value) ZEND_MIR_SAFEPOINT_CLASS_##symbol = value,
typedef enum _zend_mir_safepoint_class {
	ZEND_MIR_SAFEPOINT_CLASS_CATALOG(ZEND_MIR_SAFEPOINT_CLASS_ENUM)
	ZEND_MIR_SAFEPOINT_CLASS_COUNT = 13,
	ZEND_MIR_SAFEPOINT_CLASS_INVALID = -1
} zend_mir_safepoint_class;
#undef ZEND_MIR_SAFEPOINT_CLASS_ENUM

#define ZEND_MIR_FRAME_SLOT_KIND_CATALOG(X) \
	X(ARGUMENT, "argument", 0) \
	X(CV, "cv", 1) \
	X(TMP, "tmp", 2) \
	X(VAR, "var", 3) \
	X(RETURN_VALUE, "return_value", 4) \
	X(THIS, "this", 5) \
	X(EXTRA_ARGUMENT, "extra_argument", 6) \
	X(EXTRA_NAMED_ARGUMENT, "extra_named_argument", 7) \
	X(PENDING_CALL, "pending_call", 8)
#define ZEND_MIR_FRAME_SLOT_KIND_ENUM(symbol, label, value) ZEND_MIR_FRAME_SLOT_KIND_##symbol = value,
typedef enum _zend_mir_frame_slot_kind {
	ZEND_MIR_FRAME_SLOT_KIND_CATALOG(ZEND_MIR_FRAME_SLOT_KIND_ENUM)
	ZEND_MIR_FRAME_SLOT_KIND_COUNT = 9,
	ZEND_MIR_FRAME_SLOT_KIND_INVALID = -1
} zend_mir_frame_slot_kind;
#undef ZEND_MIR_FRAME_SLOT_KIND_ENUM

#define ZEND_MIR_FRAME_SLOT_REPRESENTATION_CATALOG(X) \
	X(CANONICAL_ZVAL, "canonical_zval", 0) \
	X(EXECUTE_DATA_FIELD, "execute_data_field", 1) \
	X(PERSISTENT_SUSPEND_STATE, "persistent_suspend_state", 2)
#define ZEND_MIR_FRAME_SLOT_REPRESENTATION_ENUM(symbol, label, value) \
	ZEND_MIR_FRAME_SLOT_REPRESENTATION_##symbol = value,
typedef enum _zend_mir_frame_slot_representation {
	ZEND_MIR_FRAME_SLOT_REPRESENTATION_CATALOG(ZEND_MIR_FRAME_SLOT_REPRESENTATION_ENUM)
	ZEND_MIR_FRAME_SLOT_REPRESENTATION_COUNT = 3,
	ZEND_MIR_FRAME_SLOT_REPRESENTATION_INVALID = -1
} zend_mir_frame_slot_representation;
#undef ZEND_MIR_FRAME_SLOT_REPRESENTATION_ENUM

#define ZEND_MIR_MATERIALIZATION_CATALOG(X) \
	X(MATERIALIZED, "materialized", 0) \
	X(UNDEF, "undef", 1) \
	X(BORROWED_POINTER, "borrowed_pointer", 2) \
	X(SOURCE_ZVAL, "source_zval", 3)
#define ZEND_MIR_MATERIALIZATION_ENUM(symbol, label, value) ZEND_MIR_MATERIALIZATION_##symbol = value,
typedef enum _zend_mir_materialization {
	ZEND_MIR_MATERIALIZATION_CATALOG(ZEND_MIR_MATERIALIZATION_ENUM)
	ZEND_MIR_MATERIALIZATION_COUNT = 4,
	ZEND_MIR_MATERIALIZATION_INVALID = -1
} zend_mir_materialization;
#undef ZEND_MIR_MATERIALIZATION_ENUM

/* This catalog intentionally differs from the general ownership-state lattice. */
#define ZEND_MIR_FRAME_SLOT_OWNERSHIP_CATALOG(X) \
	X(BORROWED, "borrowed", 0) \
	X(COPIED_REFCOUNTED, "copied_refcounted", 1) \
	X(MOVED, "moved", 2) \
	X(FRAME_OWNED, "frame_owned", 3) \
	X(CALLER_OWNED, "caller_owned", 4) \
	X(SUSPEND_STATE_OWNED, "suspend_state_owned", 5)
#define ZEND_MIR_FRAME_SLOT_OWNERSHIP_ENUM(symbol, label, value) ZEND_MIR_FRAME_SLOT_OWNERSHIP_##symbol = value,
typedef enum _zend_mir_frame_slot_ownership {
	ZEND_MIR_FRAME_SLOT_OWNERSHIP_CATALOG(ZEND_MIR_FRAME_SLOT_OWNERSHIP_ENUM)
	ZEND_MIR_FRAME_SLOT_OWNERSHIP_COUNT = 6,
	ZEND_MIR_FRAME_SLOT_OWNERSHIP_INVALID = -1
} zend_mir_frame_slot_ownership;
#undef ZEND_MIR_FRAME_SLOT_OWNERSHIP_ENUM

#define ZEND_MIR_CLEANUP_ACTION_CATALOG(X) \
	X(DESTROY, "destroy", 0) \
	X(RELEASE, "release", 1) \
	X(TRANSFER, "transfer", 2) \
	X(NONE, "none", 3)
#define ZEND_MIR_CLEANUP_ACTION_ENUM(symbol, label, value) ZEND_MIR_CLEANUP_ACTION_##symbol = value,
typedef enum _zend_mir_cleanup_action {
	ZEND_MIR_CLEANUP_ACTION_CATALOG(ZEND_MIR_CLEANUP_ACTION_ENUM)
	ZEND_MIR_CLEANUP_ACTION_COUNT = 4,
	ZEND_MIR_CLEANUP_ACTION_INVALID = -1
} zend_mir_cleanup_action;
#undef ZEND_MIR_CLEANUP_ACTION_ENUM

#define ZEND_MIR_CLEANUP_STATE_CATALOG(X) \
	X(PENDING, "pending", 0) \
	X(COMPLETE, "complete", 1) \
	X(TRANSFERRED, "transferred", 2)
#define ZEND_MIR_CLEANUP_STATE_ENUM(symbol, label, value) ZEND_MIR_CLEANUP_STATE_##symbol = value,
typedef enum _zend_mir_cleanup_state {
	ZEND_MIR_CLEANUP_STATE_CATALOG(ZEND_MIR_CLEANUP_STATE_ENUM)
	ZEND_MIR_CLEANUP_STATE_COUNT = 3,
	ZEND_MIR_CLEANUP_STATE_INVALID = -1
} zend_mir_cleanup_state;
#undef ZEND_MIR_CLEANUP_STATE_ENUM

#define ZEND_MIR_CONTINUATION_KIND_CATALOG(X) \
	X(NATIVE, "native", 0) \
	X(ZEND_EXCEPTION, "zend_exception", 1) \
	X(NONLOCAL_BAILOUT, "nonlocal_bailout", 2) \
	X(TERMINAL, "terminal", 3)
#define ZEND_MIR_CONTINUATION_KIND_ENUM(symbol, label, value) ZEND_MIR_CONTINUATION_KIND_##symbol = value,
typedef enum _zend_mir_continuation_kind {
	ZEND_MIR_CONTINUATION_KIND_CATALOG(ZEND_MIR_CONTINUATION_KIND_ENUM)
	ZEND_MIR_CONTINUATION_KIND_COUNT = 4,
	ZEND_MIR_CONTINUATION_KIND_INVALID = -1
} zend_mir_continuation_kind;
#undef ZEND_MIR_CONTINUATION_KIND_ENUM

typedef struct _zend_mir_span {
	uint32_t offset;
	uint32_t count;
} zend_mir_span;

typedef struct _zend_mir_source_position_ref {
	zend_mir_source_position_id id;
	zend_mir_symbol_id file_symbol_id;
	uint32_t line;
	uint32_t column_start;
	uint32_t column_end;
} zend_mir_source_position_ref;

/* Stable source association; no process address or generated-code location. */
typedef struct _zend_mir_source_map_ref {
	zend_mir_source_map_id id;
	zend_mir_source_position_id source_position_id;
	zend_mir_op_array_id op_array_id;
	uint32_t opline_index;
	zend_mir_opline_phase opline_phase;
	zend_mir_frame_state_id owner_frame_id;
} zend_mir_source_map_ref;

typedef struct _zend_mir_frame_slot_ref {
	uint32_t slot_id;
	zend_mir_value_id value_id;
	uint32_t index;
	zend_mir_frame_slot_kind kind;
	zend_mir_frame_slot_representation representation;
	zend_mir_materialization materialization;
	zend_mir_frame_slot_ownership ownership;
	bool rooted;
	bool cleanup_required;
} zend_mir_frame_slot_ref;

typedef struct _zend_mir_cleanup_ref {
	uint32_t slot_id;
	zend_mir_cleanup_action action;
	zend_mir_cleanup_state state;
} zend_mir_cleanup_ref;

typedef struct _zend_mir_continuation_ref {
	zend_mir_continuation_kind kind;
	zend_mir_frame_state_id frame_state_id;
	uint32_t opline_index;
} zend_mir_continuation_ref;

typedef struct _zend_mir_resume_ref {
	bool allowed;
	zend_mir_resume_entry_kind entry_kind;
	zend_mir_resume_id resume_id;
	uint32_t code_version_id;
	uint32_t target_opline_index;
} zend_mir_resume_ref;

typedef struct _zend_mir_frame_state_ref {
	zend_mir_frame_state_id id;
	zend_mir_function_id function_id;
	zend_mir_frame_state_id parent_id;
	zend_mir_function_kind function_kind;
	uint32_t opline_index;
	zend_mir_opline_phase opline_phase;
	zend_mir_span slots;
	zend_mir_span roots;
	zend_mir_span cleanup_obligations;
	zend_mir_continuation_ref return_continuation;
	zend_mir_continuation_ref exception_continuation;
	zend_mir_continuation_ref bailout_continuation;
	zend_mir_suspend_kind suspend_kind;
	uint32_t suspend_state_id;
	uint32_t code_version_id;
	zend_mir_resume_ref resume;
	zend_mir_safepoint_class safepoint_class;
	bool canonical;
} zend_mir_frame_state_ref;

#endif /* ZEND_MIR_FRAME_STATE_H */
