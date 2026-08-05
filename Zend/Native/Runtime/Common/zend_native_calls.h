/* Native user-function entry cells and Zend-frame call helpers. */

#ifndef ZEND_NATIVE_CALLS_H
#define ZEND_NATIVE_CALLS_H

#include "Zend/Native/Lowering/zend_mir_lowering_source.h"
#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _zend_native_entry_cell_state {
	ZEND_NATIVE_ENTRY_UNCOMPILED = 0,
	ZEND_NATIVE_ENTRY_COMPILING = 1,
	ZEND_NATIVE_ENTRY_READY = 2,
	ZEND_NATIVE_ENTRY_FAILED = 3
} zend_native_entry_cell_state;

typedef void (*zend_native_frame_probe_t)(
	void *context,
	const zend_execute_data *caller,
	const zend_execute_data *callee);

/*
 * Entry cells are process-local indirections. The code pointer is the publish
 * word: NULL is not ready and a non-NULL acquire load pins one fully immutable
 * code version for the duration of an activation. The owner may replace or
 * clear the word only with release ordering and retires the old mapping after
 * active and suspended users reach quiescence.
 */
typedef struct _zend_native_entry_cell {
	zend_native_entry_cell_state state;
	zend_function *function;
	const zend_native_code * volatile code;
	uint64_t generation;
	uint32_t active_calls;
	uint32_t suspended_frames;
	uint64_t published_epoch;
	uint64_t retired_epoch;
	zend_native_frame_probe_t frame_probe;
	void *frame_probe_context;
	bool lease_managed;
} zend_native_entry_cell;

static zend_always_inline const zend_native_code *
zend_native_entry_cell_load(const zend_native_entry_cell *cell)
{
	return cell == NULL ? NULL : __atomic_load_n(&cell->code, __ATOMIC_ACQUIRE);
}

static zend_always_inline bool zend_native_entry_cell_is_ready(
	const zend_native_entry_cell *cell)
{
	return zend_native_entry_cell_load(cell) != NULL;
}

static zend_always_inline void zend_native_entry_cell_retain_active(
	zend_native_entry_cell *cell)
{
	if (!cell->lease_managed) {
		cell->active_calls++;
	}
}

static zend_always_inline void zend_native_entry_cell_release_active(
	zend_native_entry_cell *cell)
{
	if (!cell->lease_managed) {
		ZEND_ASSERT(cell->active_calls != 0);
		cell->active_calls--;
	}
}

static zend_always_inline void zend_native_entry_cell_retain_suspended(
	zend_native_entry_cell *cell)
{
	if (!cell->lease_managed) {
		cell->suspended_frames++;
	}
}

static zend_always_inline void zend_native_entry_cell_release_suspended(
	zend_native_entry_cell *cell)
{
	if (!cell->lease_managed) {
		ZEND_ASSERT(cell->suspended_frames != 0);
		cell->suspended_frames--;
	}
}

typedef struct _zend_native_reentry_binding {
	zend_function *function;
	zend_native_entry_cell *entry_cell;
} zend_native_reentry_binding;

typedef zend_native_entry_cell *(*zend_native_reentry_resolver_t)(
	void *context, zend_function *function);

typedef struct _zend_native_reentry_scope {
	const zend_native_reentry_binding *bindings;
	uint32_t binding_count;
	zend_native_reentry_resolver_t resolver;
	void *resolver_context;
	struct _zend_native_reentry_scope *previous;
} zend_native_reentry_scope;

typedef enum _zend_native_internal_receiver_kind {
	ZEND_NATIVE_INTERNAL_RECEIVER_NONE = 0,
	ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS = 1,
	ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE = 2,
	ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT = 3
} zend_native_internal_receiver_kind;

/* Process-local binding for one compile-time resolved internal function. */
typedef struct _zend_native_internal_call_cell {
	zend_function *function;
	zend_class_entry *called_scope;
	zend_native_internal_receiver_kind receiver_kind;
} zend_native_internal_call_cell;

typedef enum _zend_native_call_argument_mode {
	ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE = 0,
	ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE = 1,
	ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER = 2
} zend_native_call_argument_mode;

/*
 * Immutable call-site data used by the direct Native-to-Native path. Source
 * positions remain diagnostics only; operand identity is carried explicitly.
 */
typedef struct _zend_native_direct_call_argument {
	uint32_t ordinal;
	zend_native_call_argument_mode mode;
	zend_mir_scalar_type_mask exact_type;
	uint64_t scalar_bits;
	uint32_t source_frame_offset;
	zend_mir_source_operand_ref source_operand;
} zend_native_direct_call_argument;

#define ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME UINT32_C(1)
#define ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER UINT32_C(2)
#define ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE UINT32_C(4)
#define ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME UINT32_C(8)
#define ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT UINT32_C(32)
#define ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED UINT32_C(128)

typedef struct _zend_native_direct_call_descriptor {
	uint32_t argument_count;
	/* PHP-visible argument count after resolving fixed named parameters. */
	uint32_t frame_argument_count;
	uint32_t callee_argument_count;
	uint32_t callee_compiled_variable_count;
	uint32_t callee_temporary_count;
	uint32_t default_literal_count;
	uint32_t source_position;
	uint32_t flags;
	uint32_t frame_size;
	zend_function *expected_function;
	zend_class_entry *called_scope;
	zend_native_internal_receiver_kind receiver_kind;
	zend_mir_source_operand_ref receiver_operand;
	uint32_t receiver_source_frame_offset;
	zend_mir_scalar_type_mask result_type;
	zend_mir_source_operand_ref result_operand;
	zend_native_direct_call_argument arguments[1];
} zend_native_direct_call_descriptor;

static zend_always_inline uint32_t *
zend_native_direct_call_default_literals(
	zend_native_direct_call_descriptor *descriptor)
{
	return (uint32_t *) (descriptor->arguments + descriptor->argument_count);
}

static zend_always_inline const uint32_t *
zend_native_direct_call_default_literals_const(
	const zend_native_direct_call_descriptor *descriptor)
{
	return (const uint32_t *) (
		descriptor->arguments + descriptor->argument_count);
}

/*
 * Complete immutable source-call semantics for one direct internal call.
 * The compiler resolves the INIT/SEND/DO sequence once. Runtime code uses
 * these explicit operands and payloads and consults source_position only for
 * EX(opline), diagnostics, observers, and exception routing.
 */
typedef struct _zend_native_direct_internal_call_argument {
	uint32_t ordinal;
	zend_native_call_argument_mode mode;
	uint32_t source_opcode;
	uint32_t source_position;
	zend_mir_source_operand_ref source_operand;
	zend_mir_source_operand_ref auxiliary_operand;
	uint32_t auxiliary_payload;
	uint32_t result_payload;
	uint32_t extended_value;
} zend_native_direct_internal_call_argument;

typedef struct _zend_native_direct_internal_call_descriptor {
	uint32_t argument_count;
	uint32_t initial_argument_count;
	uint32_t init_source_position;
	uint32_t do_source_position;
	uint32_t do_opcode;
	uint32_t do_op1_payload;
	uint32_t do_extended_value;
	uint32_t flags;
	zend_mir_source_operand_ref receiver_operand;
	zend_mir_source_operand_ref do_op2;
	zend_mir_source_operand_ref result_operand;
	zend_mir_scalar_type_mask result_type;
	zend_native_direct_internal_call_argument arguments[1];
} zend_native_direct_internal_call_descriptor;

#define ZEND_NATIVE_DIRECT_INTERNAL_CALL_REQUIRE_SCALAR_RESULT UINT32_C(1)

/*
 * Complete immutable source-call semantics for a user call that cannot use
 * the fixed direct-frame path. The compiler decodes INIT/SEND/DO once; runtime
 * source positions are used only to publish EX(opline) and preserve declaring
 * source identity.
 */
typedef struct _zend_native_user_call_descriptor {
	uint32_t argument_count;
	uint32_t initial_argument_count;
	uint32_t init_source_position;
	uint32_t do_source_position;
	uint32_t init_opcode;
	uint32_t do_opcode;
	uint32_t init_op1_payload;
	uint32_t init_op2_payload;
	uint32_t init_result_payload;
	uint32_t init_extended_value;
	uint32_t do_op1_payload;
	uint32_t do_op2_payload;
	uint32_t do_result_payload;
	uint32_t do_extended_value;
	zend_mir_source_operand_ref init_op1;
	zend_mir_source_operand_ref init_op2;
	zend_mir_source_operand_ref init_result;
	zend_mir_source_operand_ref do_op1;
	zend_mir_source_operand_ref do_op2;
	zend_mir_source_operand_ref do_result;
	zend_mir_scalar_type_mask result_type;
	uint32_t flags;
	zend_native_direct_internal_call_argument arguments[1];
} zend_native_user_call_descriptor;

#define ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT UINT32_C(1)

typedef enum _zend_native_user_call_resolution_status {
	ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE = 0,
	ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS = 1
} zend_native_user_call_resolution_status;

typedef enum _zend_native_check_func_arg_result {
	ZEND_NATIVE_CHECK_FUNC_ARG_BY_VALUE = 0,
	ZEND_NATIVE_CHECK_FUNC_ARG_BY_REFERENCE = 1,
	ZEND_NATIVE_CHECK_FUNC_ARG_EXCEPTION = 2
} zend_native_check_func_arg_result;

typedef enum _zend_native_user_call_target_kind {
	ZEND_NATIVE_USER_CALL_TARGET_INVALID = 0,
	ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER = 1,
	ZEND_NATIVE_USER_CALL_TARGET_INTERNAL = 2,
	/* A constructor-less NEW still uses a zend_pass_function frame so every
	 * SEND is evaluated, transferred, and destroyed with ordinary ownership. */
	ZEND_NATIVE_USER_CALL_TARGET_NO_CALL = 3,
	ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE = 4
} zend_native_user_call_target_kind;

#define ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO UINT32_MAX

#define ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT UINT32_C(1)
#define ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE UINT32_C(2)
#define ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE UINT32_C(4)
#define ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE UINT32_C(8)
#define ZEND_NATIVE_USER_CALL_OWNS_EXTRA_NAMED_PARAMS UINT32_C(16)

/*
 * One target-dependent placement decision for a source argument. The storage
 * is supplied by generated code and remains borrowed by the resolution; the
 * resolver never allocates a placement array per call.
 */
typedef struct _zend_native_user_call_placement {
	uint32_t source_index;
	uint32_t target_index;
	uint32_t mode;
	uint32_t flags;
} zend_native_user_call_placement;

#define ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID UINT32_MAX
#define ZEND_NATIVE_USER_CALL_PLACEMENT_NAMED UINT32_C(1)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_VARIADIC UINT32_C(2)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF UINT32_C(4)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF UINT32_C(8)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION UINT32_C(16)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_EXTRA_NAMED UINT32_C(32)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_REF_CHECK UINT32_C(64)
#define ZEND_NATIVE_USER_CALL_PLACEMENT_SOURCE_EVALUATED UINT32_C(128)

#define ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION UINT32_C(1)
#define ZEND_NATIVE_USER_CALL_PLACEMENTS_EXTRA_NAMED UINT32_C(2)
#define ZEND_NATIVE_USER_CALL_PLACEMENTS_MAY_HAVE_UNDEF UINT32_C(4)
#define ZEND_NATIVE_USER_CALL_PLACEMENTS_HAS_DEFAULTS UINT32_C(8)
#define ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT UINT32_C(16)
#define ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_STARTED UINT32_C(32)

/*
 * Frame-less result of resolving one universal call. SUCCESS always names one
 * explicit target kind and owns every resource named by ownership until
 * generated code transfers those bits to a published frame/activation or calls
 * release_user_resolution(). FAILURE returns a zeroed, ownership-free result.
 *
 * A TRAMPOLINE result keeps the lookup trampoline in function and the eventual
 * magic method in normalized_function. Its frame_size is large enough for both
 * the raw arguments and the normalized two-argument magic call. After raw
 * argument placement, normalize_user_resolution() packs the arguments, frees
 * the trampoline, and changes the result to NATIVE_USER or INTERNAL.
 */
typedef struct _zend_native_user_call_resolution {
	zend_function *function;
	zend_function *normalized_function;
	void *object_or_called_scope;
	zend_object *owned_target;
	zend_native_entry_cell *entry_cell;
	const zend_native_code *code;
	zend_native_frame_entry_t frame_entry;
	zend_native_frame_entry_t invoke_entry;
	zend_native_user_call_placement *placements;
	zend_array *extra_named_params;
	uint32_t target_kind;
	uint32_t call_info;
	uint32_t argument_count;
	uint32_t direct_argument_count;
	uint32_t placement_count;
	uint32_t placement_flags;
	/* Exact bytes reserved for the eventual callee frame. The universal setup
	 * activation is already stored in its preceding fake setup frame. */
	uint32_t frame_size;
	/* Retained for ABI layout stability. Universal calls expose no trailing callee
	 * activation; reservation_size is therefore identical to frame_size. */
	uint32_t activation_size;
	uint32_t reservation_size;
	uint32_t ownership;
} zend_native_user_call_resolution;

/*
 * Legacy direct activations live immediately after their Zend frame. Universal
 * setup activations instead live in a fake VM-stack frame before resolution and
 * before the eventual callee, so call-frame growth cannot invalidate them.
 * Generated callers link either form before any bailout-capable transition so
 * the outermost C-only boundary can unwind without per-call catchers.
 */
typedef struct _zend_native_direct_activation {
	zend_execute_data *setup_frame;
	zend_execute_data *caller;
	zend_execute_data *callee;
	zend_execute_data *pending_call;
	zend_native_entry_cell *cell;
	const zend_native_code *code;
	const void *descriptor;
	struct _zend_native_direct_activation *previous;
	zend_native_user_call_resolution resolution;
	zval discarded_return;
	uint32_t setup_size;
	uint32_t placement_capacity;
	uint32_t status;
	bool uses_discarded_return;
	bool raw_arguments_owned;
	bool frame_initialized;
	bool frame_requires_finish;
	bool cell_active;
	bool generator_created;
	bool dynamic_target;
	bool internal_target;
	bool setup_record;
	bool fiber_published;
} zend_native_direct_activation;

typedef struct _zend_native_direct_call_result {
	uint64_t status;
	uint64_t payload;
} zend_native_direct_call_result;

typedef struct _zend_native_direct_call_entry {
	zend_execute_data *callee;
	zend_native_frame_entry_t entry;
} zend_native_direct_call_entry;

typedef struct _zend_native_user_opcode_result {
	uint64_t action;
	uint64_t source_position;
} zend_native_user_opcode_result;

void zend_native_entry_cell_init(
	zend_native_entry_cell *cell, zend_function *function);
zend_result zend_native_entry_cell_begin_compile(zend_native_entry_cell *cell);
zend_result zend_native_entry_cell_publish(
	zend_native_entry_cell *cell, const zend_native_code *code);
/* Also rolls back a partially published, inactive recursive component. */
void zend_native_entry_cell_fail(zend_native_entry_cell *cell);
zend_result zend_native_entry_cell_reset(zend_native_entry_cell *cell);
void zend_native_entry_cell_set_frame_probe(
	zend_native_entry_cell *cell,
	zend_native_frame_probe_t probe,
	void *context);

/*
 * Reentry scopes are thread-local and stack-disciplined. The process-wide
 * native executor remains installed for the process lifetime and resolves
 * nested userland calls through the active scope without changing
 * zend_execute_ex.
 */
zend_result zend_native_reentry_startup(void);
void zend_native_reentry_shutdown(void);
zend_result zend_native_reentry_scope_enter(
	zend_native_reentry_scope *scope,
	const zend_native_reentry_binding *bindings,
	uint32_t binding_count);
zend_result zend_native_reentry_scope_enter_resolver(
	zend_native_reentry_scope *scope,
	const zend_native_reentry_binding *bindings,
	uint32_t binding_count,
	zend_native_reentry_resolver_t resolver,
	void *resolver_context);
zend_result zend_native_reentry_scope_enter_resolver_direct(
	zend_native_reentry_scope *scope,
	const zend_native_reentry_binding *bindings,
	uint32_t binding_count,
	zend_native_reentry_resolver_t resolver,
	void *resolver_context);
void zend_native_reentry_scope_leave(zend_native_reentry_scope *scope);
zend_native_entry_cell *zend_native_reentry_resolve(
	zend_function *function);

/*
 * Replace a CALL_VIA_TRAMPOLINE user frame with its concrete __call() or
 * __callStatic() target and repack the original arguments into the magic-call
 * pair. The caller remains responsible for (re)initializing or invoking the
 * resulting frame.
 */
bool zend_native_call_normalize_trampoline(
	zend_execute_data *call, zend_function *expected_target);

/*
 * Resolve and pin a universal dynamic target without creating or publishing a
 * Zend frame. The resolver may allocate, call object/class handlers, reenter,
 * throw, or bail out while performing lookup/RTC/reentry work. It retains every
 * target resource and entry-cell lease in the returned ownership mask. It never
 * transfers call arguments and never invokes or finishes a call. It resolves
 * target-dependent positional/named/variadic/by-reference placement into the
 * buffer embedded after the already-linked setup activation. AUTO conservatively
 * uses the descriptor's known initial argument count. Source reference state is
 * intentionally deferred until each SEND source point. Insufficient capacity
 * fails without allocating a plan.
 */
/* Slow stack-growth transition only. It marks the returned fake frame
 * ZEND_CALL_ALLOCATED; generated code initializes the remaining fields and
 * links the setup record with direct stores. The setup storage contains a fake
 * Zend frame header, an aligned activation, N placement rows, then at least
 * 2*N+1 uint32 target-set entries. */
void *zend_native_frame_activation_reserve(uint32_t setup_size);
zend_native_user_call_resolution_status zend_native_call_resolve_user(
	zend_native_direct_activation *activation,
	zend_native_entry_cell *entry_cell_hint,
	uint32_t argument_count_hint);
/* Bailout-unwind primitive. Normal generated completion unlinks and pops the
 * activation with direct stores and bounded ownership-release helpers. */
void zend_native_frame_activation_release(
	zend_native_direct_activation *activation);
/* Canonically pops an already-unlinked, ownership-free setup record. Unlike
 * activation_release(), this bounded helper performs no zval destruction or
 * target release; it only handles the inline/page-crossing VM-stack detail. */
void zend_native_frame_activation_pop(
	zend_native_direct_activation *activation);

/*
 * Executes all source-ordered argument operations marked for runtime expansion
 * in one bounded transition. The helper receives compiler-decoded descriptors;
 * it does not decode operands from an opline, invoke an opcode handler, or enter
 * a VM dispatcher. Array/Traversable expansion may allocate, call iterator
 * methods, reenter, throw, or bail out. The returned frame may differ after VM
 * stack growth. NULL denotes failure with caller->call retaining cleanup state.
 */
zend_execute_data *zend_native_call_expand_user_arguments(
	zend_native_direct_activation *activation);
/*
 * Execute one source-backed runtime-tail SEND at its original source phase.
 * The placement is marked consumed only after success, and the final bounded
 * expansion helper skips consumed placements so source expressions, unpacking,
 * and by-reference diagnostics are never repeated at DO.
 */
zend_result zend_native_call_send_resolved_argument(
	zend_native_direct_activation *activation, uint32_t placement_index);
/*
 * Normalize one resolved trampoline after generated code placed its raw
 * arguments in callee. This bounded helper may allocate/destruct, mutate the
 * pending frame, throw, or bail out; it does not invoke the target or dispatch
 * opcodes. On success resolution becomes NATIVE_USER or INTERNAL.
 */
zend_native_user_call_resolution_status
zend_native_call_normalize_user_resolution(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_execute_data *callee,
	zend_native_user_call_resolution *resolution);
/*
 * Releases only the resources still named by resolution->ownership. Object
 * destruction may invoke user code, reenter, throw, or bail out; generated code
 * must therefore publish all live state before calling this helper.
 */
void zend_native_call_release_user_resolution(
	zend_native_user_call_resolution *resolution);
zend_native_user_opcode_result zend_native_user_opcode_invoke(
	zend_execute_data *execute_data,
	zend_native_execution_context *context,
	uint32_t source_position_id);
/*
 * Prepare defaults, argument types, and the variadic aggregate for an already
 * initialized user frame. Constant resolution and value destruction may
 * allocate, call user code, reenter, throw, or bail out. This helper does not
 * notify observers, invoke the callee, or establish an inner bailout catcher.
 */
zend_result zend_native_frame_prepare(zend_execute_data *execute_data);
/*
 * Initialize one raw published dynamic user frame and then apply
 * zend_native_frame_prepare(). The resolver guarantees an initialized RTC, so
 * zend_init_func_execute_data performs only frame-local moves/stores before the
 * ownership flags change atomically. Defaults/type/variadic preparation after
 * that boundary may allocate, destruct, call user code, reenter, throw, or bail.
 * It does not notify observers, invoke/finalize/pop the frame, or catch bailout.
 */
zend_result zend_native_call_prepare_dynamic_frame(
	zend_native_direct_activation *activation);
/*
 * Bounded observer notifications for a fully published frame. Observer
 * callbacks may reenter, throw, or bail out. Neither helper invokes the callee,
 * prepares/cleans its frame, dispatches opcodes, or establishes a catcher.
 */
zend_native_status zend_native_frame_observer_begin(
	zend_native_direct_activation *activation);
zend_native_status zend_native_frame_observer_end(
	zend_native_direct_activation *activation, zend_native_status status);
zend_native_status zend_native_call_frameless_internal(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);

/*
 * begin may extend the VM stack and initialize a real Zend call frame. The
 * generated caller remains current after it returns. Argument setters mutate
 * only that pending frame. invoke_finish re-enters native code, restores the
 * caller on every normal/exception/bailout outcome, destroys the pending
 * scalar arguments/result and releases the frame before returning.
 */
void zend_native_call_begin(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor);
void zend_native_call_set_integer_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type);
void zend_native_call_set_double_argument(
	zend_execute_data *caller, uint32_t ordinal, double value);
uint64_t zend_native_call_invoke_finish(
	zend_execute_data *caller, zend_native_entry_cell *cell);
zend_native_status zend_native_call_invoke_finish_source(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor);
/*
 * Completes only ZEND_CALLABLE_CONVERT(_PARTIAL) for caller->call. The
 * operands are compiler-decoded source identities; source_position is used
 * only to publish EX(opline) and the partial's declaring opline. This helper
 * does not invoke the pending target or dispatch an opcode. It consumes the
 * pending frame on conversion or pre-existing-exception cleanup. Closure and
 * partial creation may allocate or throw; target destruction may invoke user
 * code, reenter, throw, or bail out. The helper does not notify observers.
 */
zend_native_status zend_native_call_convert_explicit(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint32_t op1_payload,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_position);
/* Builds the pending call frame from an immutable user-call descriptor and
 * converts it without source-opline operand decoding or opcode dispatch. */
zend_native_status zend_native_call_convert_descriptor_explicit(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor);
/*
 * Reserves a new VM-stack page for an as-yet uninitialized dynamic frame. The
 * returned frame start and caller->call are the same published address. Stack
 * growth may allocate or bail out; it does not inspect or copy an existing
 * frame, destroy values, throw, notify observers, reenter, invoke the target,
 * or dispatch an opcode.
 */
zend_execute_data *zend_native_call_reserve_dynamic_frame(
	zend_execute_data *caller, uint32_t reservation_size);
zend_native_direct_call_result zend_native_call_direct(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context);
zend_native_direct_call_entry zend_native_call_direct_enter(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context);
zend_native_direct_call_result zend_native_call_direct_leave(
	zend_execute_data *caller,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context,
	zend_native_status status);
zend_native_direct_call_entry zend_native_call_dynamic_enter(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor,
	zend_native_execution_context *context);
zend_native_direct_call_result zend_native_call_dynamic_leave(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_native_execution_context *context,
	zend_native_status status);
void zend_native_execution_context_init(
	zend_native_execution_context *context);
/*
 * Native direct activations live inside the active fiber's Zend VM stack.
 * Fiber switches therefore save and clear the thread-local chain before the
 * C-stack transfer and restore the saved chain when that fiber resumes.
 */
void *zend_native_call_fiber_suspend(void);
void zend_native_call_fiber_resume(void *active_direct_call);
void zend_native_call_fiber_destroy(void);
void zend_native_call_fiber_abandon(void *active_direct_call);
void zend_native_call_direct_unwind(zend_execute_data *outermost);
/*
 * Discard activations below a caught fatal-bailout boundary without invoking
 * destructors. Zend's bailout path marks the request as unclean and abandons
 * live frame values; cleanup here would double-destroy values already being
 * visited by GC. This helper only restores call links, active-entry accounting,
 * and VM-stack storage. It does not allocate, throw, reenter, or bail out.
 */
void zend_native_call_direct_abandon(zend_execute_data *outermost);
void zend_native_cleanup_unfinished_exception(
	zend_execute_data *execute_data, uint32_t throw_op_num,
	uint32_t catch_op_num);
void zend_native_execution_cleanup_frame(zend_execute_data *execute_data);
/*
 * Finalize one returned dynamic frame: verify its return type, service the
 * interrupt, and clean CV/argument/symbol-table state. It does not invoke the
 * target, transport the result, unlink an activation, restore caller state,
 * pop the frame, allocate a per-call object, or establish a bailout catcher.
 */
zend_native_status zend_native_execution_finish_direct_frame(
	zend_execute_data *execute_data, zend_native_status status);
zend_native_status zend_native_frame_finalize(
	zend_native_direct_activation *activation, zend_native_status status);

/*
 * Mirror the VM's exception cleanup for a call that aborted an active finally
 * body: destroy an incomplete return value and chain each delayed exception
 * behind the newly thrown exception before control transfers to an outer
 * catch/finally or caller.
 */
zend_result zend_native_prepare_finally_exception(
	zend_execute_data *caller, uint32_t source_opline_index);

zend_result zend_native_internal_call_cell_init(
	zend_native_internal_call_cell *cell,
	zend_function *function,
	zend_class_entry *called_scope,
	zend_native_internal_receiver_kind receiver_kind);
zend_result zend_native_internal_call_begin(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor);
zend_result zend_native_call_set_zval_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	const zval *value,
	zend_native_call_argument_mode mode);
zend_result zend_native_call_set_source_argument(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_index);
zend_result zend_native_direct_internal_call_set_source_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_descriptor *descriptor,
	uint32_t argument_index);
zend_result zend_native_direct_internal_call_set_integer_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_descriptor *descriptor,
	uint32_t argument_index,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type);
zend_result zend_native_direct_internal_call_set_double_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_descriptor *descriptor,
	uint32_t argument_index,
	double value);
zend_result zend_native_call_set_explicit_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument);
/* Publish a moved pending frame and synchronize the active universal setup
 * record before any following destructor or user-code transition. caller->call
 * must still name the pre-move frame for the same logical call. */
void zend_native_call_publish_moved_frame(
	zend_execute_data *caller, zend_execute_data *call);

zend_native_direct_call_result zend_native_call_fragment(
	zend_execute_data *caller,
	const zend_native_entry_cell *entry_cell,
	const zend_native_internal_call_cell *internal_cell,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t source_position);
zend_native_status zend_native_call_fragment_explicit(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint64_t encoded_op1,
	uint32_t op1_payload,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t result_payload,
	uint32_t extended_value,
	uint32_t source_position);
zend_native_status zend_native_call_check_func_arg(
	zend_execute_data *caller,
	uint64_t encoded_op1,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_opcode,
	uint32_t source_position);
zend_native_check_func_arg_result zend_native_call_check_func_arg_resolved(
	zend_native_direct_activation *activation,
	uint32_t placement_index);
zend_native_status zend_native_call_check_undef_args(
	zend_execute_data *caller,
	uint64_t encoded_op1,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_opcode,
	uint32_t source_position);
zend_native_status zend_native_internal_call_invoke_finish(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	zval *return_value);
/* Execute an already-linked top-level internal frame while leaving ownership
 * of the frame allocation with the zend_execute_ex caller. */
zend_native_status zend_native_internal_call_execute_top(
	zend_execute_data *call);
zend_native_status zend_native_internal_call_invoke_finish_source(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor);
zend_native_direct_call_result zend_native_internal_call_direct(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor);
uint64_t zend_native_call_read_source_scalar(
	zend_execute_data *caller,
	uint64_t result_operand,
	zend_mir_scalar_type_mask exact_type);
zval *zend_native_call_explicit_slot(
	zend_execute_data *caller,
	uint64_t encoded_operand,
	uint8_t *operand_type);
zval *zend_native_call_explicit_operand(
	zend_execute_data *caller,
	uint64_t encoded_operand,
	uint8_t *operand_type);
zend_native_status zend_native_return_source_zval(
	zend_execute_data *execute_data,
	uint32_t source_position,
	uint64_t encoded_operand,
	uint32_t source_opcode,
	uint32_t extended_value);
uint32_t zend_native_catch_enter(
	zend_execute_data *execute_data, uint32_t catch_opline_index);
uint32_t zend_native_catch_explicit(
	zend_execute_data *execute_data,
	uint64_t encoded_class,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_position);
zend_native_status zend_native_receive_explicit(
	zend_execute_data *execute_data,
	uint32_t source_opcode,
	uint32_t argument_number,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t source_position);
/*
 * Reload the pending callee from caller->call and perform one explicit RECV.
 * The helper may allocate/destruct, resolve constants, invoke user code through
 * reentrant destruction or lookup, throw, or bail out while applying receive
 * semantics. It neither creates a frame nor dispatches an opcode handler.
 */
zend_native_status zend_native_receive_explicit_pending(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint32_t argument_number,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t source_position);
zend_native_status zend_native_finally_enter(
	zend_execute_data *execute_data, uint32_t finally_opline_index);
void zend_native_finally_call(
	zend_execute_data *execute_data, uint32_t fast_call_opline_index);
uint32_t zend_native_finally_return(
	zend_execute_data *execute_data, uint32_t fast_ret_opline_index);
uint32_t zend_native_finally_return_explicit(
	zend_execute_data *execute_data,
	uint64_t encoded_operand,
	uint32_t try_catch_offset,
	uint32_t source_position);
zend_native_status zend_native_discard_exception(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);

#define ZEND_NATIVE_FINALLY_EXCEPTION_FLAG UINT32_C(0x80000000)
#define ZEND_NATIVE_FINALLY_GENERATOR_RETURNED (UINT32_MAX - UINT32_C(1))
#define ZEND_NATIVE_FINALLY_PROPAGATE UINT32_MAX
void zend_native_interrupt_poll(
	zend_execute_data *execute_data, uint32_t source_opline_index);

void zend_native_echo_integer(
	zend_execute_data *execute_data,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type);
void zend_native_echo_double(
	zend_execute_data *execute_data, double value);

/*
 * Fixed-signature ABI probe used by the native integration extension. It is
 * bounded, cannot allocate, call user code, throw, bail out, or reenter PHP.
 * The wide signature deliberately crosses GP/FP register and stack argument
 * boundaries on both supported targets. A zero return reports a mismatch.
 */
uint64_t zend_native_abi_conformance(
	zend_execute_data *execute_data,
	const zval *first_argument_slot,
	uint64_t source_value,
	uint8_t zext8,
	int8_t sext8,
	uint16_t zext16,
	int16_t sext16,
	uint32_t zext32,
	int32_t sext32,
	uint64_t unsigned64,
	int64_t signed64,
	uint64_t spill_a,
	uint64_t spill_b,
	double fp0,
	double fp1,
	double fp2,
	double fp3,
	double fp4,
	double fp5,
	double fp6,
	double fp7,
	double fp8,
	double fp9);

#define ZEND_NATIVE_ABI_CONFORMANCE_RESULT UINT64_C(1)

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_CALLS_H */
