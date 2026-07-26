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
	bool execute_hook_installed;
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
#define ZEND_NATIVE_DIRECT_CALL_INLINE_LEAF_BODY UINT32_C(16)
#define ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT UINT32_C(32)
#define ZEND_NATIVE_DIRECT_CALL_INLINE_BOXED_LEAF_BODY UINT32_C(64)

typedef enum _zend_native_inline_leaf_operation {
	ZEND_NATIVE_INLINE_LEAF_NONE = 0,
	ZEND_NATIVE_INLINE_LEAF_VOID = 1,
	ZEND_NATIVE_INLINE_LEAF_ARGUMENT = 2,
	ZEND_NATIVE_INLINE_LEAF_LONG_ADD_CONSTANT = 3,
	ZEND_NATIVE_INLINE_LEAF_LONG_SUB_CONSTANT = 4,
	ZEND_NATIVE_INLINE_LEAF_CONSTANT_SUB_LONG = 5,
	ZEND_NATIVE_INLINE_LEAF_SCALAR_CONSTANT = 6,
	ZEND_NATIVE_INLINE_LEAF_LONG_ADD_ARGUMENT = 7,
	ZEND_NATIVE_INLINE_LEAF_LONG_SUB_ARGUMENT = 8,
	ZEND_NATIVE_INLINE_LEAF_STRING_LENGTH_ARGUMENT = 9
} zend_native_inline_leaf_operation;

typedef struct _zend_native_direct_call_descriptor {
	uint32_t argument_count;
	uint32_t source_position;
	uint32_t flags;
	uint32_t frame_size;
	zend_native_inline_leaf_operation inline_leaf_operation;
	uint32_t inline_leaf_argument;
	uint32_t inline_leaf_argument2;
	uint64_t inline_leaf_constant;
	zend_function *expected_function;
	zend_class_entry *called_scope;
	zend_native_internal_receiver_kind receiver_kind;
	zend_mir_source_operand_ref receiver_operand;
	zend_mir_scalar_type_mask result_type;
	zend_mir_source_operand_ref result_operand;
	zend_native_direct_call_argument arguments[1];
} zend_native_direct_call_descriptor;

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
	uint32_t flags;
	zend_mir_source_operand_ref receiver_operand;
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

/*
 * A direct activation lives immediately after its Zend frame on the VM stack.
 * Generated callers link it before entering native code so the outermost
 * C-only bailout boundary can unwind every nested native frame without adding
 * another catcher to each call.
 */
typedef struct _zend_native_direct_activation {
	zend_execute_data *caller;
	zend_execute_data *callee;
	zend_execute_data *pending_call;
	zend_native_entry_cell *cell;
	const zend_native_code *code;
	const void *descriptor;
	struct _zend_native_direct_activation *previous;
	zval discarded_return;
	uint32_t status;
	bool uses_discarded_return;
	bool raw_arguments_owned;
	bool frame_initialized;
	bool frame_requires_finish;
	bool cell_active;
	bool dynamic_target;
	bool internal_target;
	bool preserve_target;
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
 * The execute hook is process-wide and reference-counted across active
 * thread-local, stack-disciplined component scopes. While a scope is active,
 * every userland reentry must resolve to one of its ready entry cells;
 * unknown targets are rejected instead of being dispatched by the VM.
 */
zend_result zend_native_reentry_startup(void);
void zend_native_reentry_shutdown(void);
zend_result zend_native_reentry_install(void);
void zend_native_reentry_uninstall(void);
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
zend_native_user_opcode_result zend_native_user_opcode_invoke(
	zend_execute_data *execute_data,
	zend_native_execution_context *context,
	uint32_t source_position_id);
zend_result zend_native_frame_prepare(zend_execute_data *execute_data);
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
void zend_native_call_direct_unwind(zend_execute_data *outermost);
void zend_native_execution_cleanup_frame(zend_execute_data *execute_data);
zend_native_status zend_native_execution_finish_direct_frame(
	zend_execute_data *execute_data, zend_native_status status);

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
zend_result zend_native_call_set_explicit_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument);
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
zend_native_status zend_native_catch_enter(
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
