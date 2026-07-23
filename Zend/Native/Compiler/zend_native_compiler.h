#ifndef ZEND_NATIVE_COMPILER_H
#define ZEND_NATIVE_COMPILER_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _zend_script zend_script;
typedef struct _zend_native_compiler zend_native_compiler;

typedef enum _zend_native_codeunit_state {
	ZEND_NATIVE_CODEUNIT_UNSEEN = 0,
	ZEND_NATIVE_CODEUNIT_COMPILING,
	ZEND_NATIVE_CODEUNIT_READY,
	ZEND_NATIVE_CODEUNIT_FAILED,
	ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
} zend_native_codeunit_state;

typedef enum _zend_native_compile_phase {
	ZEND_NATIVE_COMPILE_PHASE_SSA = 0,
	ZEND_NATIVE_COMPILE_PHASE_LOWERING,
	ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
	ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
	ZEND_NATIVE_COMPILE_PHASE_EXECUTE
} zend_native_compile_phase;

typedef enum _zend_native_compile_fault {
	ZEND_NATIVE_COMPILE_FAULT_NONE = 0,
	ZEND_NATIVE_COMPILE_FAULT_SSA,
	ZEND_NATIVE_COMPILE_FAULT_MODULE_ALLOCATION,
	ZEND_NATIVE_COMPILE_FAULT_MODULE_FINALIZE,
	ZEND_NATIVE_COMPILE_FAULT_STAGE1_VERIFY,
	ZEND_NATIVE_COMPILE_FAULT_STAGE2_VERIFY,
	ZEND_NATIVE_COMPILE_FAULT_MAPPING,
	ZEND_NATIVE_COMPILE_FAULT_ENTRY_PUBLISH
} zend_native_compile_fault;

typedef struct _zend_native_compile_diagnostic {
	zend_native_compile_phase phase;
	uint32_t code;
	uint32_t source_opline;
	bool has_source_opline;
	char message[192];
} zend_native_compile_diagnostic;

typedef void (*zend_native_compile_observer_t)(
	void *context, const zend_native_compile_diagnostic *diagnostic);

/*
 * A compiler instance owns one request-/TSRM-local native code registry.
 * script and every non-dynamic op_array supplied to compile/execute are
 * borrowed for the lifetime of the compiler. Include/eval op_arrays are
 * adopted by the compiler's dynamic-code owner.
 *
 * Observer and fault controls may inspect or reject a product operation. They
 * never perform SSA, lowering, code generation, publication, lookup, reentry,
 * or execution on the compiler's behalf.
 */
typedef struct _zend_native_compiler_config {
	zend_script *script;
	zend_native_target target;
	size_t mir_chunk_size;
	zend_native_frame_probe_t frame_probe;
	void *frame_probe_context;
	zend_native_compile_observer_t observer;
	void *observer_context;
	zend_native_compile_fault fault;
	uint32_t unavailable_runtime_helper;
	bool abi_conformance_probe;
} zend_native_compiler_config;

ZEND_API zend_native_compiler *zend_native_compiler_create(
	const zend_native_compiler_config *config,
	zend_native_compile_diagnostic *diagnostic);
ZEND_API void zend_native_compiler_destroy(zend_native_compiler *compiler);

/*
 * Compile and atomically publish the complete reachable codeunit component.
 * The supplied argument signature is retained for source compatibility but
 * does not specialize the baseline entry. Exact representations come only
 * from declared types and path-valid SSA proofs.
 */
ZEND_API zend_result zend_native_compiler_compile(
	zend_native_compiler *compiler,
	zend_op_array *root,
	const zend_mir_scalar_type_mask *supplied_argument_types,
	uint32_t supplied_argument_count,
	zend_native_compile_diagnostic *diagnostic);

ZEND_API zend_native_entry_cell *zend_native_compiler_lookup(
	const zend_native_compiler *compiler, const zend_function *function);
ZEND_API zend_native_codeunit_state zend_native_compiler_codeunit_state(
	const zend_native_compiler *compiler, const zend_function *function);
ZEND_API uint32_t zend_native_compiler_codeunit_count(
	const zend_native_compiler *compiler, zend_native_codeunit_state state);

/*
 * Product execution compiles on demand, enters the product reentry scope,
 * activates dynamic include/eval ownership, and executes a real Zend frame.
 * The result is initialized on ZEND_NATIVE_RETURNED and remains owned by the
 * caller. User exceptions remain in EG(exception).
 */
ZEND_API zend_native_status zend_native_compiler_execute(
	zend_native_compiler *compiler,
	zend_function *function,
	HashTable *arguments,
	zval *result,
	zend_native_diagnostic *diagnostic);

ZEND_API uint32_t zend_native_compiler_function_count(
	const zend_native_compiler *compiler);
ZEND_API const zend_native_code *zend_native_compiler_code_at(
	const zend_native_compiler *compiler, uint32_t index);
ZEND_API const zend_native_image *zend_native_compiler_image_at(
	const zend_native_compiler *compiler, uint32_t index);
ZEND_API const zend_native_image *zend_native_compiler_image_for(
	const zend_native_compiler *compiler, const zend_function *function);
ZEND_API uint32_t zend_native_compiler_active_call_count(
	const zend_native_compiler *compiler);
ZEND_API bool zend_native_compiler_all_code_is_wx(
	const zend_native_compiler *compiler);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_COMPILER_H */
