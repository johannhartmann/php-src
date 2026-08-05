#include <stddef.h>
#include <stdint.h>

#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

_Static_assert(ZEND_NATIVE_RUNTIME_ABI_VERSION == 80u,
	"universal-call runtime ABI changed without updating its contract test");
_Static_assert(ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RESERVE == 175,
	"setup reserve helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_CHECK_FUNC_ARG_RESOLVED == 176,
	"resolved CHECK_FUNC_ARG helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_FRAME_ACTIVATION_POP == 177,
	"setup pop helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE == 178,
	"activation release helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE == 179,
	"constant include-once helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT == 180,
	"direct internal source-argument helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_USER_CALL_SEND_RESOLVED_ARGUMENT == 181,
	"resolved user-call argument helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT == 182,
	"direct internal integer-argument helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT == 183,
	"direct internal double-argument helper id changed");
_Static_assert(ZEND_NATIVE_HELPER_COUNT == 185,
	"runtime helper count changed");
_Static_assert(offsetof(zend_native_user_call_resolution, frame_size)
	< offsetof(zend_native_user_call_resolution, activation_size),
	"universal frame-size ABI order changed");
_Static_assert(offsetof(zend_native_direct_activation, resolution)
	< offsetof(zend_native_direct_activation, setup_record),
	"setup activation ABI order changed");

static void verify_signatures(void)
{
	void *(*reserve)(uint32_t) = zend_native_frame_activation_reserve;
	void (*pop)(zend_native_direct_activation *) =
		zend_native_frame_activation_pop;
	void (*release)(zend_native_direct_activation *) =
		zend_native_frame_activation_release;

	(void) reserve;
	(void) pop;
	(void) release;
}

int main(void)
{
	verify_signatures();
	return 0;
}
