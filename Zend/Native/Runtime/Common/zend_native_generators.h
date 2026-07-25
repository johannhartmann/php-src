/* Native generator lifecycle operations. */

#ifndef ZEND_NATIVE_GENERATORS_H
#define ZEND_NATIVE_GENERATORS_H

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

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

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_GENERATORS_H */
