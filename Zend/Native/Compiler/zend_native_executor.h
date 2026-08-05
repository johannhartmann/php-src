#ifndef ZEND_NATIVE_EXECUTOR_H
#define ZEND_NATIVE_EXECUTOR_H

#include "Zend/zend.h"
#include "Zend/Native/Compiler/zend_native_compiler.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

typedef struct _zend_script zend_script;

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
void zend_native_executor_prepare_shutdown(void);
ZEND_API void zend_native_executor_execute_ex(zend_execute_data *execute_data);
ZEND_API void zend_native_executor_set_frame_probe(
	zend_native_frame_probe_t probe, void *context);
ZEND_API void zend_native_executor_set_source_probe(
	zend_native_source_probe_t probe, void *context);

/*
 * Advance the process code epoch. Existing mappings remain pinned through
 * the current request and are reclaimed before the next request enters
 * userland.
 */
ZEND_API void zend_native_executor_invalidate(void);

/*
 * OPcache cold-path integration. The slot contains one pointer-free native
 * bundle for the script's selected roots. Persistence copies it verbatim;
 * process publication resolves all Zend and runtime addresses locally.
 *
 * Preload capture records only user codeunits actually entered while the
 * preload program runs. prepare_script() adds those exact roots and their
 * statically reachable SCCs to a normal source bundle. The synthetic preload
 * owner has no executable source main; prepare_preloaded_script() serializes
 * only captured roots that were moved into that owner. Every unselected
 * function and method remains lazy.
 */
ZEND_API void zend_native_executor_begin_preload_capture(void);
ZEND_API void zend_native_executor_end_preload_capture(void);
ZEND_API zend_result zend_native_executor_prepare_script(
	zend_script *script,
	zend_native_compile_diagnostic *diagnostic);
ZEND_API zend_result zend_native_executor_prepare_preloaded_script(
	zend_script *script,
	zend_native_compile_diagnostic *diagnostic);
ZEND_API zend_result zend_native_executor_register_script_owner(
	zend_op_array *op_array, const zend_script *script);
ZEND_API zend_result zend_native_executor_register_preloaded_script(
	const zend_script *script);
ZEND_API size_t zend_native_executor_persist_calc(
	const zend_op_array *op_array);
ZEND_API size_t zend_native_executor_persist(
	zend_op_array *op_array, void *memory);
ZEND_API void zend_native_executor_discard_bundle(
	zend_op_array *op_array);
ZEND_API void zend_native_executor_set_bundle_persistent(
	zend_op_array *op_array, bool persistent);
ZEND_API int zend_native_executor_op_array_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_EXECUTOR_H */
