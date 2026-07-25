#ifndef ZEND_NATIVE_EXECUTOR_H
#define ZEND_NATIVE_EXECUTOR_H

#include "Zend/zend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install the native userland executor for the process. Request activation
 * owns only process-local compiler state; published code never enters a VM
 * fallback path.
 */
ZEND_API zend_result zend_native_executor_startup(void);
ZEND_API void zend_native_executor_shutdown(void);
ZEND_API void zend_native_executor_activate(void);
ZEND_API void zend_native_executor_deactivate(void);
ZEND_API void zend_native_executor_execute_ex(zend_execute_data *execute_data);

/*
 * Advance the process code epoch. Existing mappings remain pinned through
 * the current request and are reclaimed before the next request enters
 * userland.
 */
ZEND_API void zend_native_executor_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_EXECUTOR_H */
