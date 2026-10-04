#ifndef ZEND_MIR_ZEND_SOURCE_INTERNAL_H
#define ZEND_MIR_ZEND_SOURCE_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zend_mir_zend_source.h"

#include "../../../zend_compile.h"
#include "../../../zend_type_info.h"
#include "../../../zend_vm_opcodes.h"
#include "../../../Optimizer/zend_ssa.h"
#include "../../../Optimizer/zend_optimizer_internal.h"

#define ZEND_MIR_ZEND_SOURCE_MAGIC UINT32_C(0x5a4d4653)

typedef enum _zend_mir_frontend_operand_index {
	ZEND_MIR_FRONTEND_OP1 = 0,
	ZEND_MIR_FRONTEND_OP2 = 1,
	ZEND_MIR_FRONTEND_RESULT = 2
} zend_mir_frontend_operand_index;

bool zend_mir_frontend_normalize_operand_type(
	uint8_t operand_type,
	uint32_t operand_index,
	uint8_t *normalized_type);

const zend_op_array *zend_mir_source_op_array(const zend_mir_zend_source *source);
const zend_ssa *zend_mir_source_ssa(const zend_mir_zend_source *source);
bool zend_mir_source_is_initialized(const zend_mir_zend_source *source);

void zend_mir_frontend_set_diagnostic(
	zend_mir_frontend_diagnostic *diagnostic,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	zend_mir_op_array_id op_array_id,
	uint32_t opline_index,
	uint32_t operand_index,
	uint32_t ssa_variable_id);

zend_mir_lowering_status zend_mir_frontend_validate_slots(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *slot_count);

bool zend_mir_frontend_decode_slot(
	const zend_op_array *op_array,
	const znode_op *node,
	uint8_t operand_type,
	uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind);

bool zend_mir_frontend_ssa_slot(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id,
	uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind);

void *zend_mir_frontend_build_slot_index(
	const zend_op_array *op_array, const zend_ssa *ssa);
void zend_mir_frontend_release_slot_index(void *index);
bool zend_mir_frontend_indexed_ssa_slot(
	const void *index, uint32_t ssa_variable_id, uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind);
bool zend_mir_frontend_indexed_dead_ssa_peer_slot(
	const void *index, const zend_ssa *ssa, uint32_t ssa_variable_id,
	uint32_t *slot, zend_mir_source_slot_kind *slot_kind);

bool zend_mir_frontend_literal_index(
	const zend_op_array *op_array,
	const zend_op *opline,
	const znode_op *node,
	uint32_t *literal_index);

bool zend_mir_frontend_literal_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_literal_ref *out);

bool zend_mir_frontend_canonical_literal_for_index(
	const zend_op_array *op_array,
	uint32_t index,
	zend_mir_source_literal_ref *out);

bool zend_mir_frontend_opcode_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_opcode_ref *out);
bool zend_mir_frontend_ssa_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_ref *out);
bool zend_mir_frontend_ssa_use_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_use_ref *out);
bool zend_mir_frontend_ssa_def_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_def_ref *out);

bool zend_mir_frontend_build_operand_index(zend_mir_zend_source *source);
void zend_mir_frontend_release_operand_index(void *index);

bool zend_mir_frontend_value_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out);

bool zend_mir_frontend_build_value_fact_index(
	zend_mir_zend_source *source);

void zend_mir_frontend_release_value_fact_index(void *index);

bool zend_mir_frontend_result_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out);

bool zend_mir_frontend_fact_for_ssa_with_id(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id,
	zend_mir_value_fact_id fact_id,
	zend_mir_value_fact_ref *out);

bool zend_mir_frontend_fact_payload_for_ssa(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *out);

bool zend_mir_frontend_slot_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_slot_ref *out);

bool zend_mir_frontend_source_position_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_position_ref *out);

zend_mir_lowering_status zend_mir_zend_source_preflight(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic);

zend_function *zend_mir_zend_source_resolve_user_method_call(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t init_opline_index);

zend_function *zend_mir_zend_source_resolve_monomorphic_user_method_call(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t init_opline_index);

bool zend_mir_zend_source_direct_static_call_scope(
	const zend_script *script,
	const zend_op_array *op_array,
	uint32_t init_opline_index,
	const zend_function *function,
	bool *inherit_called_scope);

/*
 * Initialize the source adapter directly from optimizer-owned storage.
 * Prerequisite filtering is supplied by a thin source-view overlay; this
 * constructor deliberately does not materialize or mutate an OpArray/SSA copy.
 */
zend_mir_lowering_status zend_mir_zend_source_init_direct(
	zend_mir_zend_source *source,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic);

bool zend_mir_zend_op_array_exception_handler(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t throwing_opline_index,
	zend_mir_source_block_id *block_id_out,
	uint32_t *catch_opline_index_out);

#endif /* ZEND_MIR_ZEND_SOURCE_INTERNAL_H */
