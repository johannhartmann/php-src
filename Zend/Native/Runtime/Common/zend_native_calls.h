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
} zend_native_entry_cell;

void zend_native_entry_cell_init(
	zend_native_entry_cell *cell, zend_function *function);
zend_result zend_native_entry_cell_begin_compile(zend_native_entry_cell *cell);
zend_result zend_native_entry_cell_publish(
	zend_native_entry_cell *cell, const zend_native_code *code);
void zend_native_entry_cell_fail(zend_native_entry_cell *cell);
zend_result zend_native_entry_cell_reset(zend_native_entry_cell *cell);
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
	uint32_t argument_count);
void zend_native_call_set_integer_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type);
void zend_native_call_set_double_argument(
	zend_execute_data *caller, uint32_t ordinal, double value);
uint64_t zend_native_call_invoke_finish(
	zend_execute_data *caller, zend_native_entry_cell *cell);

void zend_native_echo_integer(
	zend_execute_data *execute_data,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type);
void zend_native_echo_double(
	zend_execute_data *execute_data, double value);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_CALLS_H */
