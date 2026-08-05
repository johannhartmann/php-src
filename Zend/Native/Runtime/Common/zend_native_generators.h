/* Native generator lifecycle operations. */

#ifndef ZEND_NATIVE_GENERATORS_H
#define ZEND_NATIVE_GENERATORS_H

#include "Zend/zend_gc.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/zend_gc.h"

typedef struct _zend_generator zend_generator;

#ifdef __cplusplus
extern "C" {
#endif

#define ZEND_NATIVE_GENERATOR_HELPER(name) \
	zend_native_status name( \
		zend_execute_data *execute_data, \
		uint64_t op1, uint64_t op2, uint64_t result, \
		uint32_t extended_value, uint32_t source_opcode, \
		uint32_t source_position_id);

ZEND_NATIVE_GENERATOR_HELPER(zend_native_generator_create)
ZEND_NATIVE_GENERATOR_HELPER(zend_native_generator_yield)
ZEND_NATIVE_GENERATOR_HELPER(zend_native_generator_yield_from)
ZEND_NATIVE_GENERATOR_HELPER(zend_native_generator_return)

#undef ZEND_NATIVE_GENERATOR_HELPER

zend_native_status zend_native_generator_user_opcode_return(
	zend_execute_data *execute_data);

/*
 * Complete an uncaught exceptional generator resume exactly like the VM:
 * publish a NULL observer result and close the generator heap frame before
 * control returns to zend_generator_resume().
 */
void zend_native_generator_uncaught_exception(
	zend_execute_data *execute_data);

/*
 * Release the immutable code generation owned by a generator heap frame.
 * zend_generator_close() calls this exactly once on every normal, exceptional,
 * and forced-close path.
 */
void zend_native_generator_release_generation(zend_generator *generator);

/* Freeze and restore the native setup record paired with one pending Zend
 * call frame.  The generic generator code continues to own the compact call
 * frame snapshot; these hooks only transfer native resolution/ownership state
 * and the physical setup frame surrounding it. */
bool zend_native_generator_freeze_call(
	zend_generator *generator,
	zend_execute_data *execute_data,
	zend_execute_data *call,
	zend_execute_data *frozen_call);
void *zend_native_generator_restore_call_begin(
	zend_generator *generator,
	zend_execute_data *frozen_call,
	zend_execute_data *pending_call);
void zend_native_generator_restore_call_finish(
	zend_generator *generator,
	void *restore_state,
	zend_execute_data *call);
bool zend_native_generator_frozen_call_stack_gc(
	zend_generator *generator,
	zend_execute_data *call,
	zend_get_gc_buffer *gc_buffer);
void zend_native_generator_cleanup_suspended_arguments(
	zend_execute_data *execute_data);
void zend_native_generator_cleanup_call_stack(
	zend_execute_data *execute_data);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_GENERATORS_H */
