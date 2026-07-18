#include "zend_mir_zend_source_internal.h"

const zend_op_array *zend_mir_source_op_array(const zend_mir_zend_source *source)
{
	return source == NULL ? NULL : (const zend_op_array *) source->op_array;
}

const zend_ssa *zend_mir_source_ssa(const zend_mir_zend_source *source)
{
	return source == NULL ? NULL : (const zend_ssa *) source->ssa;
}

bool zend_mir_source_is_initialized(const zend_mir_zend_source *source)
{
	return source != NULL && source->initialized == ZEND_MIR_ZEND_SOURCE_MAGIC
		&& source->op_array != NULL && source->ssa != NULL;
}

void zend_mir_frontend_set_diagnostic(
	zend_mir_frontend_diagnostic *diagnostic,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	zend_mir_op_array_id op_array_id,
	uint32_t opline_index,
	uint32_t operand_index,
	uint32_t ssa_variable_id)
{
	if (diagnostic == NULL) {
		return;
	}
	diagnostic->status = status;
	diagnostic->code = code;
	diagnostic->op_array_id = op_array_id;
	diagnostic->opline_index = opline_index;
	diagnostic->operand_index = operand_index;
	diagnostic->ssa_variable_id = ssa_variable_id;
}

void zend_mir_zend_source_reset(zend_mir_zend_source *source)
{
	if (source != NULL) {
		memset(source, 0, sizeof(*source));
		source->op_array_id = ZEND_MIR_ID_INVALID;
		source->file_symbol_id = ZEND_MIR_ID_INVALID;
	}
}

static zend_mir_lowering_status zend_mir_frontend_validate_cfg(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	const zend_basic_block *block;
	uint32_t i;

	if (op_array == NULL || ssa == NULL || ssa->cfg.blocks_count != 1
			|| ssa->cfg.blocks == NULL || ssa->cfg.edges_count != 0
			|| ssa->blocks == NULL || ssa->blocks[0].phis != NULL
			|| op_array->last_try_catch != 0) {
		goto deferred;
	}
	block = &ssa->cfg.blocks[0];
	if ((block->flags & ZEND_BB_REACHABLE) == 0
			|| (block->flags & ZEND_BB_PROTECTED) != 0
			|| block->start != 0 || block->len != op_array->last
			|| block->successors_count != 0
			|| block->predecessors_count != 0) {
		goto deferred;
	}
	if (op_array->last != 0 && ssa->cfg.map == NULL) {
		goto invalid;
	}
	for (i = 0; i < op_array->last; i++) {
		if (ssa->cfg.map[i] != 0) {
			goto invalid;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;

deferred:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_DEFERRED,
		ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED, op_array_id,
		ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_DEFERRED;
invalid:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
}

zend_mir_lowering_status zend_mir_zend_source_init(
	zend_mir_zend_source *source,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_zend_source candidate;
	zend_mir_lowering_status status;
	uint32_t slot_count;
	uint32_t use_count;
	uint32_t def_count;
	uint32_t fact_count;

	if (source == NULL) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
			ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	zend_mir_zend_source_reset(source);
	zend_mir_zend_source_reset(&candidate);
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_SUCCESS, ZEND_MIRL_OK, op_array_id,
		ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);

	if (op_array == NULL || ssa == NULL || !zend_mir_id_is_valid(op_array_id)
			|| op_array_id > ZEND_MIR_ID_MAX
			|| !zend_mir_id_is_valid(file_symbol_id)
			|| file_symbol_id > ZEND_MIR_ID_MAX) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
			ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}

	status = zend_mir_frontend_validate_cfg(
		op_array, ssa, op_array_id, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_operands(
		op_array, ssa, op_array_id, diagnostic, &use_count, &def_count);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_slots(
		op_array, ssa, op_array_id, diagnostic, &slot_count);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_opcode_scope(
		op_array, op_array_id, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_literals(
		op_array, op_array_id, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_facts(
		op_array, ssa, op_array_id, diagnostic, &fact_count);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_frontend_validate_eligibility(
		op_array, ssa, op_array_id, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}

	candidate.op_array = op_array;
	candidate.ssa = ssa;
	candidate.op_array_id = op_array_id;
	candidate.file_symbol_id = file_symbol_id;
	candidate.opcode_count = op_array->last;
	candidate.ssa_count = (uint32_t) ssa->vars_count;
	candidate.ssa_use_count = use_count;
	candidate.ssa_def_count = def_count;
	candidate.literal_count = op_array->last_literal;
	candidate.slot_count = slot_count;
	candidate.value_fact_count = fact_count;
	candidate.source_position_count = op_array->last;
	candidate.initialized = ZEND_MIR_ZEND_SOURCE_MAGIC;
	*source = candidate;
	return ZEND_MIR_LOWERING_SUCCESS;
}

static uint32_t zend_mir_frontend_view_opcode_count(const void *context)
{
	const zend_mir_zend_source *source = context;

	return zend_mir_source_is_initialized(source) ? source->opcode_count : 0;
}

static bool zend_mir_frontend_view_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	return zend_mir_frontend_opcode_at(context, index, out);
}

static uint32_t zend_mir_frontend_view_ssa_count(const void *context)
{
	const zend_mir_zend_source *source = context;

	return zend_mir_source_is_initialized(source) ? source->ssa_count : 0;
}

static bool zend_mir_frontend_view_ssa_at(
	const void *context, uint32_t index, zend_mir_source_ssa_ref *out)
{
	return zend_mir_frontend_ssa_at(context, index, out);
}

static uint32_t zend_mir_frontend_view_ssa_use_count(const void *context)
{
	const zend_mir_zend_source *source = context;

	return zend_mir_source_is_initialized(source) ? source->ssa_use_count : 0;
}

static bool zend_mir_frontend_view_ssa_use_at(
	const void *context, uint32_t index, zend_mir_source_ssa_use_ref *out)
{
	return zend_mir_frontend_ssa_use_at(context, index, out);
}

static uint32_t zend_mir_frontend_view_ssa_def_count(const void *context)
{
	const zend_mir_zend_source *source = context;

	return zend_mir_source_is_initialized(source) ? source->ssa_def_count : 0;
}

static bool zend_mir_frontend_view_ssa_def_at(
	const void *context, uint32_t index, zend_mir_source_ssa_def_ref *out)
{
	return zend_mir_frontend_ssa_def_at(context, index, out);
}

static uint32_t zend_mir_frontend_view_literal_count(const void *context)
{
	const zend_mir_zend_source *source = context;

	return zend_mir_source_is_initialized(source) ? source->literal_count : 0;
}

static bool zend_mir_frontend_view_literal_at(
	const void *context, uint32_t index, zend_mir_source_literal_ref *out)
{
	return zend_mir_frontend_literal_at(context, index, out);
}

bool zend_mir_zend_source_view(
	const zend_mir_zend_source *source,
	zend_mir_lowering_source_view *out)
{
	if (!zend_mir_source_is_initialized(source) || out == NULL) {
		return false;
	}
	out->contract_version = ZEND_MIR_CONTRACT_VERSION;
	out->context = source;
	out->opcode_count = zend_mir_frontend_view_opcode_count;
	out->opcode_at = zend_mir_frontend_view_opcode_at;
	out->ssa_count = zend_mir_frontend_view_ssa_count;
	out->ssa_at = zend_mir_frontend_view_ssa_at;
	out->ssa_use_count = zend_mir_frontend_view_ssa_use_count;
	out->ssa_use_at = zend_mir_frontend_view_ssa_use_at;
	out->ssa_def_count = zend_mir_frontend_view_ssa_def_count;
	out->ssa_def_at = zend_mir_frontend_view_ssa_def_at;
	out->literal_count = zend_mir_frontend_view_literal_count;
	out->literal_at = zend_mir_frontend_view_literal_at;
	return true;
}

uint32_t zend_mir_zend_source_slot_count(const zend_mir_zend_source *source)
{
	return zend_mir_source_is_initialized(source) ? source->slot_count : 0;
}

bool zend_mir_zend_source_slot_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_slot_ref *out)
{
	return zend_mir_frontend_slot_at(source, index, out);
}

uint32_t zend_mir_zend_source_value_fact_count(
	const zend_mir_zend_source *source)
{
	return zend_mir_source_is_initialized(source) ? source->value_fact_count : 0;
}

bool zend_mir_zend_source_value_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out)
{
	return zend_mir_frontend_value_fact_at(source, index, out);
}

uint32_t zend_mir_zend_source_position_count(
	const zend_mir_zend_source *source)
{
	return zend_mir_source_is_initialized(source)
		? source->source_position_count : 0;
}

bool zend_mir_zend_source_position_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_position_ref *out)
{
	return zend_mir_frontend_source_position_at(source, index, out);
}
