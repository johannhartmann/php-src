/* Process-local implementation of the native runtime ABI. */

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const zend_native_runtime_helper zend_native_runtime_helpers[] = {
	{ZEND_NATIVE_HELPER_USER_CALL_BEGIN,
		ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE | ZEND_NATIVE_RUNTIME_EFFECT_THROW
			| ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT,
		(const void *) zend_native_call_begin},
	{ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER, 0,
		(const void *) zend_native_call_set_integer_argument},
	{ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE, 0,
		(const void *) zend_native_call_set_double_argument},
	{ZEND_NATIVE_HELPER_USER_CALL_FINISH, ZEND_NATIVE_RUNTIME_EFFECT_ALL,
		(const void *) zend_native_call_invoke_finish},
	{ZEND_NATIVE_HELPER_ECHO_INTEGER,
		ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE | ZEND_NATIVE_RUNTIME_EFFECT_DESTRUCT
			| ZEND_NATIVE_RUNTIME_EFFECT_THROW | ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT,
		(const void *) zend_native_echo_integer},
	{ZEND_NATIVE_HELPER_ECHO_DOUBLE,
		ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE | ZEND_NATIVE_RUNTIME_EFFECT_DESTRUCT
			| ZEND_NATIVE_RUNTIME_EFFECT_THROW | ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT,
		(const void *) zend_native_echo_double},
	{ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN,
		ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE | ZEND_NATIVE_RUNTIME_EFFECT_THROW
			| ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT,
		(const void *) zend_native_internal_call_begin},
	{ZEND_NATIVE_HELPER_CALL_SET_ZVAL, ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE,
		(const void *) zend_native_call_set_zval_argument},
	{ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH, ZEND_NATIVE_RUNTIME_EFFECT_ALL,
		(const void *) zend_native_internal_call_invoke_finish},
	{ZEND_NATIVE_HELPER_INTERRUPT_POLL,
		ZEND_NATIVE_RUNTIME_EFFECT_THROW | ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT
			| ZEND_NATIVE_RUNTIME_EFFECT_USERLAND
			| ZEND_NATIVE_RUNTIME_EFFECT_REENTER,
		(const void *) zend_native_interrupt_poll},
	{ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT,
		ZEND_NATIVE_RUNTIME_EFFECT_ALLOCATE,
		(const void *) zend_native_call_set_source_argument},
	{ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE,
		ZEND_NATIVE_RUNTIME_EFFECT_ALL,
		(const void *) zend_native_internal_call_invoke_finish_source},
	{ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR,
		ZEND_NATIVE_RUNTIME_EFFECT_THROW | ZEND_NATIVE_RUNTIME_EFFECT_BAILOUT,
		(const void *) zend_native_call_read_source_scalar},
	{ZEND_NATIVE_HELPER_CATCH_ENTER,
		ZEND_NATIVE_RUNTIME_EFFECT_DESTRUCT | ZEND_NATIVE_RUNTIME_EFFECT_THROW,
		(const void *) zend_native_catch_enter},
};

static const zend_native_runtime_api zend_native_runtime = {
	ZEND_NATIVE_RUNTIME_ABI_VERSION,
	sizeof(zend_native_runtime_api),
	ZEND_NATIVE_RUNTIME_CAP_USER_CALL
		| ZEND_NATIVE_RUNTIME_CAP_INTERNAL_CALL
		| ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT
		| ZEND_NATIVE_RUNTIME_CAP_OBSERVER
		| ZEND_NATIVE_RUNTIME_CAP_INTERRUPT
		| ZEND_NATIVE_RUNTIME_CAP_BAILOUT_BOUNDARY,
	zend_native_runtime_helpers,
	(uint32_t) (sizeof(zend_native_runtime_helpers)
		/ sizeof(zend_native_runtime_helpers[0])),
	0
};

static void zend_native_runtime_diagnostic(
	zend_native_diagnostic *diagnostic,
	zend_native_diagnostic_code code,
	const char *message)
{
	if (diagnostic == NULL) {
		return;
	}
	diagnostic->code = code;
	snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
}

const zend_native_runtime_api *zend_native_runtime_get(void)
{
	return &zend_native_runtime;
}

const zend_native_runtime_helper *zend_native_runtime_helper_find(
	const zend_native_runtime_api *runtime,
	zend_native_runtime_helper_id id)
{
	uint32_t index;

	if (runtime == NULL || runtime->helpers == NULL) {
		return NULL;
	}
	for (index = 0; index < runtime->helper_count; index++) {
		if (runtime->helpers[index].id == (uint32_t) id) {
			return &runtime->helpers[index];
		}
	}
	return NULL;
}

zend_result zend_native_runtime_validate(
	const zend_native_runtime_api *runtime,
	uint64_t required_capabilities,
	zend_native_diagnostic *diagnostic)
{
	uint32_t index;
	uint32_t seen = 0;

	if (runtime == NULL
			|| runtime->abi_version != ZEND_NATIVE_RUNTIME_ABI_VERSION
			|| runtime->struct_size < sizeof(zend_native_runtime_api)
			|| runtime->helpers == NULL || runtime->helper_count == 0) {
		zend_native_runtime_diagnostic(diagnostic,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native runtime ABI version or size is incompatible");
		return FAILURE;
	}
	if ((runtime->capabilities & required_capabilities)
			!= required_capabilities) {
		zend_native_runtime_diagnostic(diagnostic,
			ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
			"native runtime lacks a required capability");
		return FAILURE;
	}
	for (index = 0; index < runtime->helper_count; index++) {
		const zend_native_runtime_helper *helper = &runtime->helpers[index];

		if (helper->id == 0 || helper->id > 31 || helper->address == NULL
				|| (helper->effects & ~ZEND_NATIVE_RUNTIME_EFFECT_ALL) != 0
				|| ((helper->effects & ZEND_NATIVE_RUNTIME_EFFECT_REENTER) != 0
					&& (helper->effects
						& ZEND_NATIVE_RUNTIME_EFFECT_USERLAND) == 0)
				|| (seen & (UINT32_C(1) << helper->id)) != 0) {
			zend_native_runtime_diagnostic(diagnostic,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native runtime helper contract is invalid or contradictory");
			return FAILURE;
		}
		seen |= UINT32_C(1) << helper->id;
	}
	return SUCCESS;
}
