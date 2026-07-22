/* Versioned process-local C runtime ABI for generated native code. */

#ifndef ZEND_NATIVE_RUNTIME_H
#define ZEND_NATIVE_RUNTIME_H

#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZEND_NATIVE_RUNTIME_ABI_VERSION 2u

typedef enum _zend_native_runtime_capability {
	ZEND_NATIVE_RUNTIME_CAP_USER_CALL = UINT64_C(1) << 0,
	ZEND_NATIVE_RUNTIME_CAP_INTERNAL_CALL = UINT64_C(1) << 1,
	ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT = UINT64_C(1) << 2,
	ZEND_NATIVE_RUNTIME_CAP_OBSERVER = UINT64_C(1) << 3,
	ZEND_NATIVE_RUNTIME_CAP_INTERRUPT = UINT64_C(1) << 4,
	ZEND_NATIVE_RUNTIME_CAP_BAILOUT_BOUNDARY = UINT64_C(1) << 5
} zend_native_runtime_capability;

typedef enum _zend_native_runtime_helper_id {
	ZEND_NATIVE_HELPER_USER_CALL_BEGIN = 1,
	ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER = 2,
	ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE = 3,
	ZEND_NATIVE_HELPER_USER_CALL_FINISH = 4,
	ZEND_NATIVE_HELPER_ECHO_INTEGER = 5,
	ZEND_NATIVE_HELPER_ECHO_DOUBLE = 6,
	ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN = 7,
	ZEND_NATIVE_HELPER_CALL_SET_ZVAL = 8,
	ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH = 9,
	ZEND_NATIVE_HELPER_INTERRUPT_POLL = 10,
	ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT = 11,
	ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE = 12,
	ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR = 13,
	ZEND_NATIVE_HELPER_CATCH_ENTER = 14,
	ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE = 15,
	ZEND_NATIVE_HELPER_FINALLY_ENTER = 16,
	ZEND_NATIVE_HELPER_FINALLY_CALL = 17,
	ZEND_NATIVE_HELPER_FINALLY_RETURN = 18,
	ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL = 19
} zend_native_runtime_helper_id;

typedef enum _zend_native_runtime_effect {
	ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE = UINT32_C(1) << 0,
	ZEND_NATIVE_RUNTIME_EFFECT_DESTRUCT = UINT32_C(1) << 1,
	ZEND_NATIVE_RUNTIME_EFFECT_THROW = UINT32_C(1) << 2,
	ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT = UINT32_C(1) << 3,
	ZEND_NATIVE_RUNTIME_EFFECT_USERLAND = UINT32_C(1) << 4,
	ZEND_NATIVE_RUNTIME_EFFECT_REENTER = UINT32_C(1) << 5,
	ZEND_NATIVE_RUNTIME_EFFECT_OBSERVE = UINT32_C(1) << 6
} zend_native_runtime_effect;

#define ZEND_NATIVE_RUNTIME_EFFECT_ALL ( \
	ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE \
	| ZEND_NATIVE_RUNTIME_EFFECT_DESTRUCT \
	| ZEND_NATIVE_RUNTIME_EFFECT_THROW \
	| ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT \
	| ZEND_NATIVE_RUNTIME_EFFECT_USERLAND \
	| ZEND_NATIVE_RUNTIME_EFFECT_REENTER \
	| ZEND_NATIVE_RUNTIME_EFFECT_OBSERVE)

typedef struct _zend_native_runtime_helper {
	uint32_t id;
	uint32_t effects;
	const void *address;
} zend_native_runtime_helper;

/*
 * The table is process-local and never serialized into MIR. Generated code
 * binds stable helper IDs to these addresses while it is compiled in the
 * current process. Appending fields requires a minor ABI version bump.
 */
typedef struct _zend_native_runtime_api {
	uint32_t abi_version;
	uint32_t struct_size;
	uint64_t capabilities;
	const zend_native_runtime_helper *helpers;
	uint32_t helper_count;
	uint32_t reserved;
} zend_native_runtime_api;

const zend_native_runtime_api *zend_native_runtime_get(void);
zend_result zend_native_runtime_validate(
	const zend_native_runtime_api *runtime,
	uint64_t required_capabilities,
	zend_native_diagnostic *diagnostic);
const zend_native_runtime_helper *zend_native_runtime_helper_find(
	const zend_native_runtime_api *runtime,
	zend_native_runtime_helper_id id);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_RUNTIME_H */
