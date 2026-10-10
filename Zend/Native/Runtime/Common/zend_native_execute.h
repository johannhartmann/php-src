#ifndef ZEND_NATIVE_EXECUTE_H
#define ZEND_NATIVE_EXECUTE_H

/* The C boundary of native frames (zend_native_execute.c). */

#include "Zend/zend_compile.h"
#include "Zend/zend_dtrace.h"
#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

/*
 * The state lives in the C frame of the caller of the function that calls
 * setjmp(), so the catcher never reads an indeterminate automatic after
 * longjmp. It must not live on the VM stack: nothing may follow the newest
 * frame there, because a tier-2 host entry grows that frame in place. A
 * generator frame lies below its thawed call frames and leaves the VM stack
 * to them.
 */
typedef struct _zend_native_execution_state {
	zend_vm_stack previous_stack;
	zval *previous_stack_top;
	zend_native_status status;
	zval discarded_return;
	zval *original_return_value;
	bool observer_started;
	bool observer_finished;
#ifdef HAVE_DTRACE
	zend_dtrace_user_frame dtrace_frame;
	bool dtrace_frame_started;
#endif
} zend_native_execution_state;

/*
 * execute_ex's direct entry of a frame with no generator, observer or
 * DTrace probe whose cell it retained: unwound by a bailout record instead
 * of a zend_try (the cell is released when a bailout passes the frame).
 */
zend_native_status zend_native_execute_frame_entered(
	zend_native_frame_entry_t entry, zend_execute_data *execute_data,
	zend_native_entry_cell *cell);

#endif /* ZEND_NATIVE_EXECUTE_H */
