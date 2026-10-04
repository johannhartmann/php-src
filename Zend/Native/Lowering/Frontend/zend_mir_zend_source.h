/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright © The PHP Group                                            |
   +----------------------------------------------------------------------+
   | This source file is subject to the Modified BSD License that is      |
   | bundled with this package in the file LICENSE, and is available      |
   | through the world-wide-web at https://www.php.net/license/bsd-license.php |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_ZEND_SOURCE_H
#define ZEND_MIR_ZEND_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "../zend_mir_lowering_diagnostic.h"
#include "../zend_mir_lowering_source.h"
#include "../../Calls/Contracts/zend_mir_call_source.h"
#include "../../MIR/zend_mir_frame_state.h"
#include "../../MIR/zend_mir_scalar.h"

struct _zend_op_array;
struct _zend_ssa;
struct _zend_script;
union _zend_function;

#define ZEND_MIR_FRONTEND_OPERAND_NONE ZEND_MIR_ID_INVALID

typedef struct _zend_mir_frontend_diagnostic {
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code code;
	zend_mir_op_array_id op_array_id;
	uint32_t opline_index;
	uint32_t operand_index;
	uint32_t ssa_variable_id;
} zend_mir_frontend_diagnostic;

typedef struct _zend_mir_source_slot_ref {
	uint32_t slot_id;
	zend_mir_source_slot_kind kind;
	uint32_t kind_index;
} zend_mir_source_slot_ref;

/*
 * Process-local, caller-owned adapter state. The input pointers are borrowed
 * only while callbacks are used. They never appear in source-view records.
 */
typedef struct _zend_mir_zend_source {
	const void *op_array;
	const void *ssa;
	const void *call_op_array;
	const void *call_ssa;
	const void *script;
	void *call_inventory;
	void *phi_index;
	void *operand_index;
	void *slot_index;
	void *original_slot_index;
	void *value_fact_index;
	void *block_region_flags;
	void *block_loop_headers;
	zend_mir_op_array_id op_array_id;
	zend_mir_symbol_id file_symbol_id;
	uint32_t opcode_count;
	uint32_t ssa_count;
	uint32_t ssa_use_count;
	uint32_t ssa_def_count;
	uint32_t literal_count;
	uint32_t slot_count;
	uint32_t value_fact_count;
	uint32_t base_value_fact_count;
	uint32_t source_position_count;
	uint32_t block_count;
	uint32_t edge_count;
	uint32_t phi_count;
	uint32_t phi_input_count;
	uint32_t call_site_count;
	uint32_t call_target_count;
	uint32_t call_argument_count;
	uint32_t call_parameter_mode_count;
	bool calls_enabled;
	uint32_t initialized;
} zend_mir_zend_source;

void zend_mir_zend_source_reset(zend_mir_zend_source *source);

/*
 * Add the process-local call inventory to an initialized source.
 * The original (unprojected) op-array and SSA remain borrowed until release.
 */
zend_mir_lowering_status zend_mir_zend_source_enable(
	zend_mir_zend_source *source,
	const struct _zend_script *script,
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic);

void zend_mir_zend_source_release(zend_mir_zend_source *source);

bool zend_mir_zend_source_call_view(
	const zend_mir_zend_source *source,
	zend_mir_source_call_view *out);

bool zend_mir_zend_source_call_target_resolver(
	const zend_mir_zend_source *source,
	zend_mir_source_call_target_resolver *out);

/* Proves that a RETURN reads the exact source zval written by a direct
 * internal call. Protected handlers may lack optimizer SSA, so this proof is
 * intentionally based on original oplines and physical source slots. */
bool zend_mir_zend_source_return_source_zval(
	const zend_mir_zend_source *source,
	uint32_t return_opline_index);

/* Resolve the innermost source-backed catch entry for a throwing opline. */
bool zend_mir_zend_source_exception_handler(
	const zend_mir_zend_source *source,
	uint32_t throwing_opline_index,
	zend_mir_source_block_id *block_id_out,
	uint32_t *catch_opline_index_out);

/*
 * Resolve one source call to an exact internal function. Instance methods are
 * accepted only when SSA class evidence and finality prove that normal Zend
 * method dispatch cannot select an override.
 */
union _zend_function *zend_mir_zend_source_resolve_internal_call(
	const struct _zend_script *script,
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	uint32_t init_opline_index);

bool zend_mir_zend_source_view(
	const zend_mir_zend_source *source,
	zend_mir_lowering_source_view *out);

uint32_t zend_mir_zend_source_slot_count(const zend_mir_zend_source *source);
bool zend_mir_zend_source_slot_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_slot_ref *out);

uint32_t zend_mir_zend_source_value_fact_count(const zend_mir_zend_source *source);
bool zend_mir_zend_source_value_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out);

uint32_t zend_mir_zend_source_position_count(const zend_mir_zend_source *source);
bool zend_mir_zend_source_position_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_position_ref *out);

#endif /* ZEND_MIR_ZEND_SOURCE_H */
