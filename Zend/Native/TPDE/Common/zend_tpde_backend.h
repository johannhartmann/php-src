/* C boundary for executable ZNMIR on the supported TPDE targets. */

#ifndef ZEND_TPDE_BACKEND_H
#define ZEND_TPDE_BACKEND_H

#include "Zend/zend_types.h"
#include "Zend/Native/MIR/zend_mir.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _zend_op_array;
struct _zend_op;
struct _zend_ssa;

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
	ZEND_NATIVE_BAILOUT = 2,
	/*
	 * Internal generated-code result used by a private scalar call frame.
	 * No C boundary may observe this value: the caller immediately retries
	 * through the canonical Zend-frame path before any PHP-visible mutation.
	 */
	ZEND_NATIVE_RETRY = 3,
	/* The initial generator frame was transferred to its heap object. */
	ZEND_NATIVE_GENERATOR_CREATED = 4,
	/* A generator preserved its frame and yielded control to its caller. */
	ZEND_NATIVE_SUSPENDED = 5,
	/* The generator helper closed and released the heap frame. */
	ZEND_NATIVE_GENERATOR_RETURNED = 6
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
	const struct _zend_op **opline_before_exception;
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
	uint64_t direct_leaf_scalar_sites;
	uint64_t direct_typed_body_sites;
	uint64_t direct_call_frame_bytes;
	uint64_t inner_call_runtime_helper_calls;
	uint64_t inner_call_heap_allocations;
	uint64_t inner_call_catcher_boundaries;
} zend_native_image_metrics;

typedef enum _zend_native_image_reference_kind {
	ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL = 1,
	ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION = 2,
	ZEND_NATIVE_IMAGE_REFERENCE_CLASS = 3
} zend_native_image_reference_kind;

/*
 * Persistent images contain only stable reference tokens. The compiler owns
 * the Zend-identity mapping used to encode them and reconstructs process-local
 * addresses once, immediately before publication.
 */
typedef bool (*zend_native_image_encode_reference_t)(
	void *context,
	zend_native_image_reference_kind kind,
	const void *address,
	uint64_t *token);
typedef bool (*zend_native_image_decode_reference_t)(
	void *context,
	zend_native_image_reference_kind kind,
	uint64_t token,
	const void **address);

typedef struct _zend_native_call_binding {
	zend_mir_call_target_id target_id;
	zend_native_entry_cell *entry_cell;
	/*
	 * Index in the current multi-function TPDE component.  UINT32_MAX keeps
	 * the existing Entry-Cell path for dynamic and cross-component edges.
	 */
	uint32_t component_target_index;
	bool direct_native;
	bool leaf_scalar_frame;
} zend_native_call_binding;

typedef struct _zend_native_internal_call_binding {
	zend_mir_call_target_id target_id;
	zend_native_internal_call_cell *call_cell;
} zend_native_internal_call_binding;

typedef enum _zend_native_source_effect_kind {
	ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR = 1,
	ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE = 2,
	ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE = 3,
	ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE = 4
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

/*
 * One function in a statically known native component.  The member borrows
 * all inputs for the duration of compilation; TPDE consumes the complete
 * array in one compile invocation and emits one relocatable object containing
 * every member function.
 */
typedef struct _zend_native_component_member {
	const zend_mir_view *module;
	const zend_native_call_binding *user_bindings;
	uint32_t user_binding_count;
	const zend_native_internal_call_binding *internal_bindings;
	uint32_t internal_binding_count;
	const zend_native_source_effect *effects;
	uint32_t effect_count;
	uint32_t frame_argument_count;
	const struct _zend_op_array *source_op_array;
	const struct _zend_ssa *source_ssa;
} zend_native_component_member;

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
	const struct _zend_ssa *source_ssa,
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
	const struct _zend_ssa *source_ssa,
	const struct _zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_tpde_compile_component_w14_with_runtime(
	zend_native_target target,
	const zend_native_component_member *members,
	uint32_t member_count,
	const struct _zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);

zend_result zend_native_publish_image(
	zend_native_target target,
	zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);

zend_result zend_native_image_serialize(
	const zend_native_image *image,
	zend_native_image_encode_reference_t encode_reference,
	void *reference_context,
	unsigned char **out_bytes,
	size_t *out_size,
	zend_native_diagnostic *diag);
zend_result zend_native_image_deserialize(
	const unsigned char *bytes,
	size_t size,
	zend_native_image_decode_reference_t decode_reference,
	void *reference_context,
	zend_native_image **out_image,
	zend_native_diagnostic *diag);
void zend_native_serialized_image_destroy(unsigned char *bytes);

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
zend_result zend_native_code_component_view(
	zend_native_code *code,
	uint32_t component_index,
	zend_native_code **out_view,
	zend_native_diagnostic *diag);
const char *zend_native_target_id(zend_native_target target);
const char *zend_native_target_triple(zend_native_target target);
size_t zend_native_image_size(const zend_native_image *image);
const unsigned char *zend_native_image_bytes(const zend_native_image *image);
uint32_t zend_native_image_component_count(const zend_native_image *image);
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
