/* C boundary for executable ZNMIR on the supported TPDE targets. */

#ifndef ZEND_TPDE_BACKEND_H
#define ZEND_TPDE_BACKEND_H

#include "Zend/zend_types.h"
#include "Zend/Native/MIR/zend_mir.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _zend_op_array;

typedef enum _zend_native_target {
	ZEND_NATIVE_TARGET_DARWIN_ARM64 = 0,
	ZEND_NATIVE_TARGET_LINUX_AMD64 = 1
} zend_native_target;

typedef enum _zend_native_diagnostic_code {
	ZEND_NATIVE_DIAGNOSTIC_OK = 0,
	ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT = 1,
	ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_TARGET = 2,
	ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR = 3,
	ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE = 4,
	ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED = 5,
	ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED = 6,
	ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH = 7
} zend_native_diagnostic_code;

typedef struct _zend_native_diagnostic {
	zend_native_diagnostic_code code;
	char message[192];
} zend_native_diagnostic;

typedef enum _zend_native_scalar_kind {
	ZEND_NATIVE_SCALAR_NULL = 0,
	ZEND_NATIVE_SCALAR_BOOL = 1,
	ZEND_NATIVE_SCALAR_LONG = 2,
	ZEND_NATIVE_SCALAR_DOUBLE = 3
} zend_native_scalar_kind;

typedef struct _zend_native_scalar {
	uint64_t payload_bits;
	uint32_t kind;
	uint32_t reserved;
} zend_native_scalar;

typedef enum _zend_native_status {
	ZEND_NATIVE_RETURNED = 0,
	ZEND_NATIVE_EXCEPTION = 1,
	ZEND_NATIVE_BAILOUT = 2
} zend_native_status;

/*
 * Request-local executor addresses carried through the internal Native ABI.
 * Resolving the ZTS/NTS executor globals once at the outer C boundary lets
 * nested generated entries update the real Zend VM stack and current frame
 * without a TLS lookup or runtime transition on every Native-to-Native call.
 */
typedef struct _zend_native_execution_context {
	struct _zend_vm_stack **vm_stack;
	zval **vm_stack_top;
	zval **vm_stack_end;
	zend_execute_data **current_execute_data;
	void **active_direct_call;
	void **map_ptr_base_address;
	struct zend_atomic_bool_s *vm_interrupt;
	zend_object **exception;
	void **stack_limit;
	bool observers_enabled;
} zend_native_execution_context;

typedef zend_native_status (*zend_native_frame_entry_t)(
	zend_execute_data *execute_data,
	zend_native_execution_context *context);

typedef struct zend_native_image zend_native_image;
typedef struct zend_native_code zend_native_code;
typedef struct _zend_native_entry_cell zend_native_entry_cell;
typedef struct _zend_native_internal_call_cell zend_native_internal_call_cell;
struct _zend_native_runtime_api;

typedef struct _zend_native_image_metrics {
	uint64_t runtime_helper_sites;
	uint64_t source_opline_decode_sites;
	uint64_t guard_sites;
	uint64_t slow_path_sites;
	uint64_t direct_call_sites;
	uint64_t direct_call_frame_bytes;
} zend_native_image_metrics;

typedef struct _zend_native_call_binding {
	zend_mir_call_target_id target_id;
	zend_native_entry_cell *entry_cell;
	bool direct_native;
} zend_native_call_binding;

typedef struct _zend_native_internal_call_binding {
	zend_mir_call_target_id target_id;
	zend_native_internal_call_cell *call_cell;
} zend_native_internal_call_binding;

typedef enum _zend_native_source_effect_kind {
	ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR = 1,
	ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE = 2,
	ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE = 3
} zend_native_source_effect_kind;

/*
 * W07/W08 source effects remain process-local compiler input. They augment a
 * verified W05/W06 module without changing either persistent MIR contract.
 * source_position_id must identify exactly one scalar carrier instruction in
 * the module, preserving source order and its proven scalar type.
 */
typedef struct _zend_native_source_effect {
	zend_mir_source_position_id source_position_id;
	zend_native_source_effect_kind kind;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_block_id target_block_id;
} zend_native_source_effect;

zend_result zend_tpde_compile_module(
	zend_native_target target,
	const zend_mir_view *module,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_tpde_compile_module_bound(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_tpde_compile_module_w07(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_tpde_compile_module_w08(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const struct _zend_op_array *source_op_array,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

/*
 * Compile against an explicit process-local runtime ABI. This is the same
 * production adaptor path used by zend_tpde_compile_module_w08(); embedders
 * may supply an ABI-compatible table and every helper actually required by
 * the plan is resolved before an image can be returned.
 */
zend_result zend_tpde_compile_module_w08_with_runtime(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const struct _zend_op_array *source_op_array,
	const struct _zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_native_publish_image(
	zend_native_target target,
	zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);

zend_result zend_native_execute(
	const zend_native_code *code,
	const zend_native_scalar *arguments,
	uint32_t argument_count,
	zend_native_scalar *result,
	zend_native_diagnostic *diag);

zend_native_status zend_native_execute_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diag);

/*
 * zend_call_function() has already emitted the begin notification before it
 * enters zend_execute_ex.  Native reentry uses this variant so the native
 * boundary owns cleanup and the matching end notification without emitting a
 * duplicate begin event.
 */
zend_native_status zend_native_execute_observed_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diag);

void zend_native_image_destroy(zend_native_image *image);
void zend_native_code_destroy(zend_native_code *code);
const char *zend_native_target_id(zend_native_target target);
const char *zend_native_target_triple(zend_native_target target);
size_t zend_native_image_size(const zend_native_image *image);
const unsigned char *zend_native_image_bytes(const zend_native_image *image);
void zend_native_image_get_metrics(
	const zend_native_image *image, zend_native_image_metrics *metrics);
bool zend_native_code_is_writable(const zend_native_code *code);
bool zend_native_code_is_executable(const zend_native_code *code);
bool zend_native_code_has_unwind_info(const zend_native_code *code);
uint32_t zend_native_live_unwind_registration_count(void);
bool zend_native_code_contains_address(
	const zend_native_code *code, const void *address);
zend_native_frame_entry_t zend_native_code_frame_entry(
	const zend_native_code *code);
uint32_t zend_native_code_argument_count(const zend_native_code *code);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_TPDE_BACKEND_H */
