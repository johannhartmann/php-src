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

zend_mir_lowering_status zend_mir_frontend_validate_literals(
	const zend_op_array *op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic);

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

zend_mir_lowering_status zend_mir_frontend_validate_operands(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *use_count,
	uint32_t *def_count);
zend_mir_lowering_status zend_mir_frontend_validate_operands_w04(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *use_count,
	uint32_t *def_count);

zend_mir_lowering_status zend_mir_frontend_validate_eligibility(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic);
zend_mir_lowering_status zend_mir_frontend_validate_eligibility_w04(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic);

zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope(
	const zend_op_array *op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic);
zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope_w04(
	const zend_op_array *op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic);

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

zend_mir_lowering_status zend_mir_frontend_validate_facts(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *fact_count);

bool zend_mir_frontend_value_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out);

bool zend_mir_frontend_w05_result_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out);

zend_mir_lowering_status zend_mir_frontend_project_w05_result_facts(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic);

zend_mir_lowering_status zend_mir_frontend_project_w08_result_facts(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic);

bool zend_mir_frontend_fact_for_ssa(
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

zend_mir_lowering_status zend_mir_zend_source_preflight_w05(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic);

zend_mir_lowering_status zend_mir_zend_source_preflight_w07(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic);

zend_mir_lowering_status zend_mir_zend_source_preflight_w08(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic);

zend_mir_lowering_status zend_mir_zend_source_init_w05_projection(
	zend_mir_zend_source *source,
	const zend_op_array *projected_op_array,
	const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array,
	const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic);

#endif /* ZEND_MIR_ZEND_SOURCE_INTERNAL_H */
