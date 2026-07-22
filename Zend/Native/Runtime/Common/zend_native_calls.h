/* Native user-function entry cells and Zend-frame call helpers. */

#ifndef ZEND_NATIVE_CALLS_H
#define ZEND_NATIVE_CALLS_H

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
 * Entry cells are process-local indirections. They own neither the
 * zend_function nor native code and may be reset only after active_calls is
 * zero. Keeping the indirection stable allows recursive callers to be emitted
 * while the whole reachable component is still being compiled.
 */
typedef struct _zend_native_entry_cell {
	zend_native_entry_cell_state state;
	zend_function *function;
	const zend_native_code *code;
	uint64_t generation;
	uint32_t active_calls;
	zend_native_frame_probe_t frame_probe;
	void *frame_probe_context;
} zend_native_entry_cell;

typedef struct _zend_native_reentry_binding {
	zend_function *function;
	zend_native_entry_cell *entry_cell;
} zend_native_reentry_binding;

typedef struct _zend_native_reentry_scope {
	const zend_native_reentry_binding *bindings;
	uint32_t binding_count;
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
	ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE = 1
} zend_native_call_argument_mode;

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
void zend_native_reentry_scope_leave(zend_native_reentry_scope *scope);
zend_result zend_native_frame_prepare(zend_execute_data *execute_data);

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
	uint32_t argument_count,
	uint32_t source_opline_index);
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
	uint32_t do_opline_index);

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
	uint32_t argument_count,
	uint32_t source_opline_index);
zend_result zend_native_call_set_zval_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	const zval *value,
	zend_native_call_argument_mode mode);
zend_result zend_native_call_set_source_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	uint32_t send_opline_index,
	zend_native_call_argument_mode mode);
zend_native_status zend_native_internal_call_invoke_finish(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	zval *return_value);
zend_native_status zend_native_internal_call_invoke_finish_source(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	uint32_t do_opline_index);
uint64_t zend_native_call_read_source_scalar(
	zend_execute_data *caller,
	uint32_t do_opline_index,
	zend_mir_scalar_type_mask exact_type);
zend_native_status zend_native_return_source_zval(
	zend_execute_data *execute_data, uint32_t return_opline_index);
zend_native_status zend_native_catch_enter(
	zend_execute_data *execute_data, uint32_t catch_opline_index);
zend_native_status zend_native_finally_enter(
	zend_execute_data *execute_data, uint32_t finally_opline_index);
void zend_native_finally_call(
	zend_execute_data *execute_data, uint32_t fast_call_opline_index);
uint32_t zend_native_finally_return(
	zend_execute_data *execute_data, uint32_t fast_ret_opline_index);

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
