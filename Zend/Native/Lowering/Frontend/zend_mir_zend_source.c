#include "zend_mir_zend_source_internal.h"

#include "../../../Optimizer/zend_dfg.h"
#include "../../../zend_execute.h"
#include "../../../zend_object_handlers.h"

typedef struct _zend_mir_frontend_call_target {
	zend_mir_source_call_target_ref record;
	const zend_function *function;
} zend_mir_frontend_call_target;

typedef struct _zend_mir_frontend_call_inventory {
	zend_mir_source_call_site_ref *sites;
	zend_mir_frontend_call_target *targets;
	zend_mir_source_call_argument_ref *arguments;
	zend_mir_source_parameter_mode_ref *parameter_modes;
	uint32_t site_count;
	uint32_t target_count;
	uint32_t argument_count;
	uint32_t parameter_mode_count;
	uint32_t parameter_mode_capacity;
	uint32_t base_value_fact_count;
	uint32_t result_fact_count;
} zend_mir_frontend_call_inventory;

static bool zend_mir_frontend_is_call_init(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_frontend_is_call_do(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_frontend_is_call_send(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_PLACEHOLDER:
			return true;
		default:
			return false;
	}
}

static zend_mir_source_call_argument_mode
zend_mir_frontend_call_argument_mode(const zend_op *opline)
{
	switch (opline->opcode) {
		case ZEND_SEND_PLACEHOLDER:
			return ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER;
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
			return ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK;
		default:
			break;
	}
	if (opline->op2_type == IS_CONST) {
		return ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED;
	}
	switch (opline->opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_FUNC_ARG:
			return ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE;
		case ZEND_SEND_REF:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
			return ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE;
		default:
			return ZEND_MIR_SOURCE_CALL_ARGUMENT_MODE_INVALID;
	}
}

static void zend_mir_frontend_release_call_inventory(
	zend_mir_frontend_call_inventory *inventory)
{
	if (inventory == NULL) {
		return;
	}
	free(inventory->arguments);
	free(inventory->parameter_modes);
	free(inventory->targets);
	free(inventory->sites);
	free(inventory);
}

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

static bool zend_mir_frontend_cfg_dominates(
	const zend_cfg *cfg, uint32_t dominator, uint32_t block);
static bool zend_mir_frontend_cfg_has_valid_root(
	const zend_cfg *cfg, uint32_t block, bool allow_protected_regions);

static zend_mir_lowering_status zend_mir_frontend_validate_cfg_w04(
	const zend_op_array *op_array, const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id, zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *phi_count, uint32_t *phi_input_count,
	bool allow_protected_regions, bool allow_object_type_constraints)
{
	uint32_t i;
	uint32_t edges = 0;
	uint32_t phis = 0;
	uint32_t inputs = 0;
	if (op_array == NULL || ssa == NULL || ssa->cfg.blocks_count == 0
			|| ssa->cfg.blocks_count > ZEND_MIR_ID_MAX
			|| ssa->cfg.edges_count > ZEND_MIR_ID_MAX
			|| ssa->cfg.blocks == NULL || ssa->blocks == NULL
			|| (op_array->last != 0 && ssa->cfg.map == NULL)
			|| (ssa->cfg.flags & ZEND_CFG_STACKLESS) != 0) {
		goto malformed;
	}
	if (op_array->last_try_catch != 0 && !allow_protected_regions) {
		zend_mir_frontend_set_diagnostic(diagnostic,
			ZEND_MIR_LOWERING_DEFERRED, ZEND_MIRL_W04_PROTECTED_REGION,
			op_array_id, ZEND_MIR_ID_INVALID,
			ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	for (i = 0; i < ssa->cfg.blocks_count; i++) {
		const zend_basic_block *block = &ssa->cfg.blocks[i];
		zend_ssa_phi *phi;
		uint32_t j;
		if ((block->flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		if ((block->flags & ZEND_BB_PROTECTED) != 0
				&& !allow_protected_regions) {
			zend_mir_frontend_set_diagnostic(diagnostic,
				ZEND_MIR_LOWERING_DEFERRED, ZEND_MIRL_W04_PROTECTED_REGION,
				op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if ((block->flags & ZEND_BB_IRREDUCIBLE_LOOP) != 0) {
			zend_mir_frontend_set_diagnostic(diagnostic,
				ZEND_MIR_LOWERING_DEFERRED, ZEND_MIRL_W04_IRREDUCIBLE_LOOP,
				op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (block->start > op_array->last
				|| block->len > op_array->last - block->start
				|| (block->successors_count != 0 && block->successors == NULL)
				|| block->idom < -1
				|| (block->idom >= 0
					&& ((uint32_t) block->idom >= ssa->cfg.blocks_count
						|| (uint32_t) block->idom == i))
				|| (i == 0
					? block->idom != -1
					: block->idom < 0
						&& !allow_protected_regions)
				|| block->loop_header < -1
				|| (block->loop_header >= 0
					&& ((uint32_t) block->loop_header
							>= ssa->cfg.blocks_count
						|| (ssa->cfg.blocks[block->loop_header].flags
							& ZEND_BB_LOOP_HEADER) == 0
						|| !zend_mir_frontend_cfg_dominates(
							&ssa->cfg, (uint32_t) block->loop_header, i)))
				|| (block->predecessors_count != 0
					&& (ssa->cfg.predecessors == NULL
						|| block->predecessor_offset < 0
						|| (uint32_t) block->predecessor_offset
							> ssa->cfg.edges_count
						|| block->predecessors_count
							> ssa->cfg.edges_count
								- (uint32_t) block->predecessor_offset))) {
			goto malformed;
		}
		for (j = 0; j < block->successors_count; j++) {
			if (block->successors[j] < 0
					|| (uint32_t) block->successors[j]
						>= ssa->cfg.blocks_count
					|| (ssa->cfg.blocks[block->successors[j]].flags
						& ZEND_BB_REACHABLE) == 0) {
				goto malformed;
			}
		}
		for (j = 0; j < block->predecessors_count; j++) {
			uint32_t offset = (uint32_t) block->predecessor_offset + j;
			int predecessor = ssa->cfg.predecessors[offset];
			uint32_t successor;
			bool reciprocal = false;
			if (predecessor < 0
					|| (uint32_t) predecessor >= ssa->cfg.blocks_count
					|| (ssa->cfg.blocks[predecessor].flags
						& ZEND_BB_REACHABLE) == 0) {
				goto malformed;
			}
			for (successor = 0;
				successor < ssa->cfg.blocks[predecessor].successors_count;
				successor++) {
				if (ssa->cfg.blocks[predecessor].successors[successor]
						== (int) i) {
					reciprocal = true;
					break;
				}
			}
			if (!reciprocal) {
				goto malformed;
			}
		}
		for (j = 0; j < block->len; j++) {
			if (ssa->cfg.map[block->start + j] != i) {
				goto malformed;
			}
		}
		if (edges > UINT32_MAX - block->successors_count) {
			goto malformed;
		}
		edges += block->successors_count;
		for (phi = ssa->blocks[i].phis; phi != NULL; phi = phi->next) {
			uint32_t phi_inputs = phi->pi >= 0 ? 1 : block->predecessors_count;
			uint32_t input;
			if (phi->block != i || phi->ssa_var < 0
					|| (uint32_t) phi->ssa_var >= (uint32_t) ssa->vars_count
					|| phi->sources == NULL
					|| (phi->pi >= 0
						&& ((uint32_t) phi->pi >= ssa->cfg.blocks_count
							|| (ssa->cfg.blocks[phi->pi].flags
								& ZEND_BB_REACHABLE) == 0))
					|| (phi->has_range_constraint
						&& ((phi->constraint.range.min_ssa_var >= 0
								&& (uint32_t)
									phi->constraint.range.min_ssa_var
									>= (uint32_t) ssa->vars_count)
							|| (phi->constraint.range.max_ssa_var >= 0
								&& (uint32_t)
									phi->constraint.range.max_ssa_var
									>= (uint32_t) ssa->vars_count)))
					|| phis == UINT32_MAX
					|| inputs > UINT32_MAX - phi_inputs) {
				goto malformed;
			}
			if (phi->pi >= 0
					&& ((!phi->has_range_constraint
							&& ((!allow_object_type_constraints
									&& phi->constraint.type.ce != NULL)
								|| (phi->constraint.type.ce == NULL
									&& phi->constraint.type.type_mask == 0))))) {
				goto unsupported_phi;
			}
			for (input = 0; input < phi_inputs; input++) {
				if (phi->sources[input] < 0
						|| (uint32_t) phi->sources[input]
							>= (uint32_t) ssa->vars_count) {
					goto malformed;
				}
			}
			if (phi->pi >= 0) {
				bool predecessor_found = false;
				for (input = 0; input < block->predecessors_count; input++) {
					uint32_t offset =
						(uint32_t) block->predecessor_offset + input;
					if (ssa->cfg.predecessors[offset] == phi->pi) {
						predecessor_found = true;
						break;
					}
				}
				if (!predecessor_found) {
					goto malformed;
				}
			}
			phis++;
			inputs += phi_inputs;
		}
	}
	if (edges != ssa->cfg.edges_count) {
		goto malformed;
	}
	for (i = 0; i < ssa->cfg.blocks_count; i++) {
		if ((ssa->cfg.blocks[i].flags & ZEND_BB_REACHABLE) != 0
				&& !zend_mir_frontend_cfg_has_valid_root(
					&ssa->cfg, i, allow_protected_regions)) {
			goto malformed;
		}
	}
	for (i = 0; i < op_array->last; i++) {
		if (ssa->cfg.map[i] >= ssa->cfg.blocks_count) {
			goto malformed;
		}
	}
	*phi_count = phis;
	*phi_input_count = inputs;
	return ZEND_MIR_LOWERING_SUCCESS;
unsupported_phi:
	zend_mir_frontend_set_diagnostic(diagnostic,
		ZEND_MIR_LOWERING_DEFERRED, ZEND_MIRL_W04_UNSUPPORTED_PHI_PI,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_DEFERRED;
malformed:
	zend_mir_frontend_set_diagnostic(diagnostic,
		ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_W04_MALFORMED_CFG,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
}

static bool zend_mir_frontend_w05_original_result_slot(
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	uint32_t ssa_variable_id, uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	const zend_ssa_var *variable;
	uint32_t opline_index;
	uint32_t physical_slot;
	uint32_t physical_count;
	uint32_t cv_count;

	if (projected_op_array == NULL || projected_ssa == NULL
			|| original_op_array == NULL || original_ssa == NULL
			|| slot == NULL || slot_kind == NULL
			|| projected_ssa->vars_count != original_ssa->vars_count
			|| ssa_variable_id >= (uint32_t) projected_ssa->vars_count
			|| projected_ssa->vars == NULL || original_ssa->vars == NULL
			|| original_ssa->ops == NULL || original_op_array->opcodes == NULL
			|| projected_op_array->last_var < 0
			|| original_op_array->last_var != projected_op_array->last_var
			|| original_op_array->T != projected_op_array->T) {
		return false;
	}
	cv_count = (uint32_t) projected_op_array->last_var;
	if (cv_count > ZEND_MIR_ID_MAX - projected_op_array->T) {
		return false;
	}
	physical_count = cv_count + projected_op_array->T;
	variable = &projected_ssa->vars[ssa_variable_id];
	if (variable->var < projected_op_array->last_var
			|| (uint32_t) variable->var >= physical_count
			|| original_ssa->vars[ssa_variable_id].var != variable->var
			|| variable->definition != -1
			|| variable->definition_phi != NULL
			|| variable->use_chain != -1
			|| variable->phi_use_chain != NULL
			|| variable->sym_use_chain != NULL) {
		return false;
	}
	physical_slot =
		(uint32_t) variable->var - (uint32_t) projected_op_array->last_var;
	for (opline_index = 0; opline_index < original_op_array->last;
			opline_index++) {
		const zend_op *opline = &original_op_array->opcodes[opline_index];

		if ((opline->opcode != ZEND_DO_UCALL
					&& opline->opcode != ZEND_DO_FCALL
					&& opline->opcode != ZEND_DO_ICALL
					&& opline->opcode != ZEND_CALLABLE_CONVERT
					&& opline->opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL)
				|| !zend_mir_frontend_decode_slot(
					original_op_array, &opline->result, opline->result_type,
					slot, slot_kind)) {
			continue;
		}
		if (*slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| *slot != physical_slot) {
			continue;
		}
		if (original_ssa->ops[opline_index].result_def
				== (int) ssa_variable_id) {
			return true;
		}
		/* Zend's optimizer may leave a dead, definitionsless SSA version for
		 * a temporary that is physically owned by the call result. It maps to
		 * that same source slot but publishes no independent value fact. */
		if (original_ssa->vars[ssa_variable_id].definition == -1
				&& original_ssa->vars[ssa_variable_id].definition_phi == NULL
				&& original_ssa->vars[ssa_variable_id].use_chain == -1
				&& original_ssa->vars[ssa_variable_id].phi_use_chain == NULL
				&& original_ssa->vars[ssa_variable_id].sym_use_chain == NULL) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_frontend_dead_ssa_peer_slot(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t ssa_variable_id, uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	const zend_ssa_var *variable;
	uint32_t candidate;

	if (op_array == NULL || ssa == NULL || ssa->vars == NULL
			|| slot == NULL || slot_kind == NULL
			|| ssa_variable_id >= (uint32_t) ssa->vars_count) {
		return false;
	}
	variable = &ssa->vars[ssa_variable_id];
	if (variable->var < op_array->last_var || variable->definition != -1
			|| variable->definition_phi != NULL || variable->use_chain != -1
			|| variable->phi_use_chain != NULL
			|| variable->sym_use_chain != NULL) {
		return false;
	}
	for (candidate = 0; candidate < (uint32_t) ssa->vars_count;
			candidate++) {
		if (candidate == ssa_variable_id
				|| ssa->vars[candidate].var != variable->var) {
			continue;
		}
		if (zend_mir_frontend_ssa_slot(
				op_array, ssa, candidate, slot, slot_kind)) {
			return true;
		}
	}
	return false;
}

static zend_mir_lowering_status zend_mir_frontend_validate_slots_w05(
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic, uint32_t *slot_count)
{
	uint32_t cv_count;
	uint32_t index;
	uint32_t ignored_slot;
	zend_mir_source_slot_kind ignored_kind;

	if (slot_count == NULL || projected_op_array == NULL
			|| projected_ssa == NULL || original_op_array == NULL
			|| original_ssa == NULL || projected_op_array->last_var < 0
			|| projected_ssa->vars_count != original_ssa->vars_count) {
		goto invalid;
	}
	cv_count = (uint32_t) projected_op_array->last_var;
	if (projected_op_array->T > (ZEND_MIR_ID_MAX - cv_count) / 2) {
		goto invalid;
	}
	*slot_count = cv_count + projected_op_array->T * 2;
	for (index = 0; index < (uint32_t) projected_ssa->vars_count; index++) {
		if (!zend_mir_frontend_ssa_slot(
				projected_op_array, projected_ssa, index,
				&ignored_slot, &ignored_kind)
				&& !zend_mir_frontend_dead_ssa_peer_slot(
					projected_op_array, projected_ssa, index,
					&ignored_slot, &ignored_kind)
				&& !zend_mir_frontend_ssa_slot(
					original_op_array, original_ssa, index,
					&ignored_slot, &ignored_kind)
				&& !zend_mir_frontend_dead_ssa_peer_slot(
					original_op_array, original_ssa, index,
					&ignored_slot, &ignored_kind)
				&& !zend_mir_frontend_w05_original_result_slot(
					projected_op_array, projected_ssa,
					original_op_array, original_ssa, index,
					&ignored_slot, &ignored_kind)) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id,
				ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE, index);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;

invalid:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
}

static zend_mir_lowering_status zend_mir_zend_source_init_w04_impl(
	zend_mir_zend_source *source, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id, zend_mir_frontend_diagnostic *diagnostic,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	bool allow_protected_regions, bool allow_any_source_zval_return,
	bool allow_w10_operations)
{
	zend_mir_zend_source candidate;
	zend_mir_lowering_status status;
	uint32_t slots, uses, defs, facts, phis, inputs;
	if (source == NULL || op_array == NULL || ssa == NULL
			|| !zend_mir_id_is_valid(op_array_id)
			|| !zend_mir_id_is_valid(file_symbol_id)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	zend_mir_zend_source_reset(source);
	status = zend_mir_frontend_validate_cfg_w04(
		op_array, ssa, op_array_id, diagnostic, &phis, &inputs,
		allow_protected_regions, allow_w10_operations);
	if (status != ZEND_MIR_LOWERING_SUCCESS
			|| (status = zend_mir_frontend_validate_operands_w04(
				op_array, ssa, op_array_id, diagnostic, &uses, &defs))
				!= ZEND_MIR_LOWERING_SUCCESS
			|| (status = original_op_array == NULL && original_ssa == NULL
				? zend_mir_frontend_validate_slots(
					op_array, ssa, op_array_id, diagnostic, &slots)
				: zend_mir_frontend_validate_slots_w05(
					op_array, ssa, original_op_array, original_ssa,
					op_array_id, diagnostic, &slots))
				!= ZEND_MIR_LOWERING_SUCCESS
			|| (status = allow_w10_operations
				? zend_mir_frontend_validate_opcode_scope_w10(
					op_array, op_array_id, diagnostic)
				: allow_any_source_zval_return
				? zend_mir_frontend_validate_opcode_scope_w09(
					op_array, op_array_id, diagnostic)
				: zend_mir_frontend_validate_opcode_scope_w04(
					op_array, op_array_id, diagnostic))
				!= ZEND_MIR_LOWERING_SUCCESS
			|| (status = zend_mir_frontend_validate_literals(
				op_array, op_array_id, diagnostic))
				!= ZEND_MIR_LOWERING_SUCCESS
			|| (status = zend_mir_frontend_validate_facts(
				op_array, ssa, op_array_id, diagnostic, &facts))
				!= ZEND_MIR_LOWERING_SUCCESS
			|| (status = allow_w10_operations
				? zend_mir_frontend_validate_eligibility_w10(
					op_array, ssa, original_op_array, op_array_id, diagnostic)
				: allow_any_source_zval_return
				? zend_mir_frontend_validate_eligibility_w09(
					op_array, ssa, original_op_array, op_array_id, diagnostic)
				: allow_protected_regions
				? zend_mir_frontend_validate_eligibility_w08(
					op_array, ssa, op_array_id, diagnostic)
				: zend_mir_frontend_validate_eligibility_w04(
					op_array, ssa, op_array_id, diagnostic))
				!= ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	memset(&candidate, 0, sizeof(candidate));
	candidate.op_array = op_array;
	candidate.ssa = ssa;
	candidate.op_array_id = op_array_id;
	candidate.file_symbol_id = file_symbol_id;
	candidate.opcode_count = op_array->last;
	candidate.ssa_count = (uint32_t) ssa->vars_count;
	candidate.ssa_use_count = uses;
	candidate.ssa_def_count = defs;
	candidate.literal_count = op_array->last_literal;
	candidate.slot_count = slots;
	candidate.value_fact_count = facts;
	candidate.source_position_count = op_array->last;
	candidate.block_count = ssa->cfg.blocks_count;
	candidate.edge_count = ssa->cfg.edges_count;
	candidate.phi_count = phis;
	candidate.phi_input_count = inputs;
	candidate.w04 = true;
	candidate.initialized = ZEND_MIR_ZEND_SOURCE_MAGIC;
	*source = candidate;
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_zend_source_init_w04(
	zend_mir_zend_source *source, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_init_w04_impl(
		source, op_array, ssa, op_array_id, file_symbol_id, diagnostic,
		NULL, NULL, false, false, false);
}

zend_mir_lowering_status zend_mir_zend_source_init_w05_projection(
	zend_mir_zend_source *source,
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id, zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_init_w04_impl(
		source, projected_op_array, projected_ssa, op_array_id, file_symbol_id,
		diagnostic, original_op_array, original_ssa, false, false, false);
}

zend_mir_lowering_status zend_mir_zend_source_init_w08_projection(
	zend_mir_zend_source *source,
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id, zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_lowering_status status = zend_mir_zend_source_init_w04_impl(
		source, projected_op_array, projected_ssa, op_array_id, file_symbol_id,
		diagnostic, original_op_array, original_ssa, true, false, false);
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		source->w08 = true;
	}
	return status;
}

zend_mir_lowering_status zend_mir_zend_source_init_w09_projection(
	zend_mir_zend_source *source,
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id, zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_lowering_status status = zend_mir_zend_source_init_w04_impl(
		source, projected_op_array, projected_ssa, op_array_id, file_symbol_id,
		diagnostic, original_op_array, original_ssa, true, true, false);
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		source->w08 = true;
		source->w09 = true;
	}
	return status;
}

zend_mir_lowering_status zend_mir_zend_source_init_w10_projection(
	zend_mir_zend_source *source,
	const zend_op_array *projected_op_array, const zend_ssa *projected_ssa,
	const zend_op_array *original_op_array, const zend_ssa *original_ssa,
	zend_mir_op_array_id op_array_id, zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_lowering_status status = zend_mir_zend_source_init_w04_impl(
		source, projected_op_array, projected_ssa, op_array_id, file_symbol_id,
		diagnostic, original_op_array, original_ssa, true, true, true);
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		source->w08 = true;
		source->w09 = true;
		source->w10 = true;
	}
	return status;
}

zend_mir_lowering_status zend_mir_zend_source_init_w11_direct(
	zend_mir_zend_source *source,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_symbol_id file_symbol_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_zend_source candidate;
	zend_mir_lowering_status status;
	uint32_t slots;
	uint32_t phis;
	uint32_t inputs;

	if (source == NULL || op_array == NULL || ssa == NULL
			|| !zend_mir_id_is_valid(op_array_id)
			|| !zend_mir_id_is_valid(file_symbol_id)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	zend_mir_zend_source_reset(source);
	status = zend_mir_frontend_validate_cfg_w04(
		op_array, ssa, op_array_id, diagnostic, &phis, &inputs, true, true);
	if (status != ZEND_MIR_LOWERING_SUCCESS
			|| (status = zend_mir_frontend_validate_slots(
				op_array, ssa, op_array_id, diagnostic, &slots))
				!= ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	memset(&candidate, 0, sizeof(candidate));
	candidate.op_array = op_array;
	candidate.ssa = ssa;
	candidate.op_array_id = op_array_id;
	candidate.file_symbol_id = file_symbol_id;
	candidate.opcode_count = op_array->last;
	candidate.ssa_count = (uint32_t) ssa->vars_count;
	candidate.literal_count = op_array->last_literal;
	candidate.slot_count = slots;
	candidate.source_position_count = op_array->last;
	candidate.block_count = ssa->cfg.blocks_count;
	candidate.edge_count = ssa->cfg.edges_count;
	candidate.phi_count = phis;
	candidate.phi_input_count = inputs;
	candidate.w04 = true;
	candidate.w08 = true;
	candidate.w09 = true;
	candidate.w10 = true;
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
	const zend_mir_zend_source *source = context;
	const zend_op_array *projected_op_array;
	const zend_ssa *projected_ssa;

	if (zend_mir_frontend_ssa_at(source, index, out)) {
		return true;
	}
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| source->call_op_array == NULL || source->call_ssa == NULL
			|| out == NULL || index >= source->ssa_count) {
		return false;
	}
	projected_op_array = zend_mir_source_op_array(source);
	projected_ssa = zend_mir_source_ssa(source);
	out->ssa_variable_id = index;
	out->definition_opline_index = ZEND_MIR_ID_INVALID;
	if (zend_mir_frontend_ssa_slot(
			(const zend_op_array *) source->call_op_array,
			(const zend_ssa *) source->call_ssa, index,
			&out->source_slot, &out->source_slot_kind)) {
		return true;
	}
	if (zend_mir_frontend_dead_ssa_peer_slot(
			(const zend_op_array *) source->call_op_array,
			(const zend_ssa *) source->call_ssa, index,
			&out->source_slot, &out->source_slot_kind)) {
		return true;
	}
	return zend_mir_frontend_w05_original_result_slot(
		projected_op_array, projected_ssa,
		source->call_op_array, source->call_ssa, index,
		&out->source_slot, &out->source_slot_kind);
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

static uint32_t zend_mir_frontend_view_block_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w04
		? source->block_count : 0;
}

static uint32_t zend_mir_frontend_view_edge_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w04
		? source->edge_count : 0;
}

static uint32_t zend_mir_frontend_view_phi_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w04
		? source->phi_count : 0;
}

static uint32_t zend_mir_frontend_view_phi_input_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w04
		? source->phi_input_count : 0;
}

static bool zend_mir_frontend_view_block_at(
	const void *context, uint32_t index, zend_mir_source_block_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	const zend_basic_block *block;
	if (!zend_mir_source_is_initialized(source) || !source->w04
			|| out == NULL || index >= source->block_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	block = &ssa->cfg.blocks[index];
	memset(out, 0, sizeof(*out));
	out->id = index;
	out->first_opcode_ordinal = block->start;
	out->opcode_count = block->len;
	if (index == 0) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_ENTRY;
	}
	if ((block->flags & ZEND_BB_REACHABLE) != 0) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_REACHABLE;
	}
	if ((block->flags & ZEND_BB_LOOP_HEADER) != 0) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	}
	if ((block->flags & ZEND_BB_PROTECTED) != 0
			|| (op_array->last_try_catch != 0 && index != 0
				&& block->idom < 0)) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_PROTECTED;
	}
	if ((block->flags & ZEND_BB_IRREDUCIBLE_LOOP) != 0) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE;
	}
	if (block->len != 0 && op_array->last_try_catch != 0) {
		uint32_t try_index;
		for (try_index = 0; try_index < op_array->last_try_catch; try_index++) {
			const zend_try_catch_element *region =
				&op_array->try_catch_array[try_index];
			if (region->catch_op != 0 && region->catch_op == block->start) {
				out->flags |= ZEND_MIR_SOURCE_BLOCK_CATCH_ENTRY;
			}
			if (region->finally_op != 0 && region->finally_op == block->start) {
				out->flags |= ZEND_MIR_SOURCE_BLOCK_FINALLY_ENTRY;
			}
		}
	}
	if (source->w08 && source->w05 && source->call_op_array != NULL
			&& block->len != 0
			&& block->start < ((const zend_op_array *) source->call_op_array)->last
			&& ((const zend_op_array *) source->call_op_array)
				->opcodes[block->start].opcode == ZEND_CATCH) {
		out->flags |= ZEND_MIR_SOURCE_BLOCK_CATCH_ENTRY;
	}
	out->immediate_dominator =
		block->idom < 0 ? ZEND_MIR_ID_INVALID : (uint32_t) block->idom;
	out->loop_header =
		block->loop_header < 0 ? ZEND_MIR_ID_INVALID : (uint32_t) block->loop_header;
	return true;
}

bool zend_mir_zend_op_array_exception_handler(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t throwing_opline_index,
	zend_mir_source_block_id *block_id_out,
	uint32_t *catch_opline_index_out)
{
	uint32_t selected = ZEND_MIR_ID_INVALID;
	uint32_t handler_opline = ZEND_MIR_ID_INVALID;
	uint32_t index;

	if (op_array == NULL || ssa == NULL
			|| block_id_out == NULL || catch_opline_index_out == NULL
			|| throwing_opline_index >= op_array->last) {
		return false;
	}
	/*
	 * Mirror zend_dispatch_try_catch_finally_helper's table order using only
	 * stable source indices. The last containing table entry is innermost.
	 * A throw before catch enters catch; a throw after catch but before finally
	 * enters finally; a throw from inside that finally continues outward.
	 */
	for (index = 0; index < op_array->last_try_catch; index++) {
		const zend_try_catch_element *region =
			&op_array->try_catch_array[index];
		if (region->try_op <= throwing_opline_index
				&& ((region->catch_op != 0
						&& throwing_opline_index < region->catch_op)
					|| (region->finally_end != 0
						&& throwing_opline_index < region->finally_end))) {
			selected = index;
		}
	}
	while (zend_mir_id_is_valid(selected)) {
		const zend_try_catch_element *region =
			&op_array->try_catch_array[selected];
		if (region->catch_op != 0
				&& throwing_opline_index < region->catch_op) {
			handler_opline = region->catch_op;
			break;
		}
		if (region->finally_op != 0
				&& throwing_opline_index < region->finally_op) {
			handler_opline = region->finally_op;
			break;
		}
		if (selected == 0) {
			selected = ZEND_MIR_ID_INVALID;
		} else {
			selected--;
		}
	}
	if (!zend_mir_id_is_valid(handler_opline)) {
		return false;
	}
	*catch_opline_index_out = handler_opline;
	if (*catch_opline_index_out >= op_array->last
			|| ssa->cfg.map == NULL) {
		return false;
	}
	*block_id_out = ssa->cfg.map[*catch_opline_index_out];
	return *block_id_out < ssa->cfg.blocks_count;
}

bool zend_mir_zend_source_exception_handler(
	const zend_mir_zend_source *source,
	uint32_t throwing_opline_index,
	zend_mir_source_block_id *block_id_out,
	uint32_t *catch_opline_index_out)
{
	if (!zend_mir_source_is_initialized(source) || !source->w04) {
		return false;
	}
	return zend_mir_zend_op_array_exception_handler(
		zend_mir_source_op_array(source), zend_mir_source_ssa(source),
		throwing_opline_index, block_id_out, catch_opline_index_out);
}

static bool zend_mir_frontend_cfg_dominates(
	const zend_cfg *cfg, uint32_t dominator, uint32_t block)
{
	uint32_t remaining = cfg->blocks_count;
	while (remaining-- != 0) {
		int idom;
		if (block == dominator) {
			return true;
		}
		idom = cfg->blocks[block].idom;
		if (idom < 0 || (uint32_t) idom == block
				|| (uint32_t) idom >= cfg->blocks_count) {
			return false;
		}
		block = (uint32_t) idom;
	}
	return false;
}

static bool zend_mir_frontend_cfg_has_valid_root(
	const zend_cfg *cfg, uint32_t block, bool allow_protected_regions)
{
	uint32_t remaining = cfg->blocks_count;

	while (remaining-- != 0) {
		int idom;

		if (block == 0) {
			return cfg->blocks[block].idom < 0;
		}
		idom = cfg->blocks[block].idom;
		if (idom < 0) {
			return allow_protected_regions;
		}
		if ((uint32_t) idom == block
				|| (uint32_t) idom >= cfg->blocks_count) {
			return false;
		}
		block = (uint32_t) idom;
	}
	return false;
}

static bool zend_mir_frontend_view_edge_at(
	const void *context, uint32_t index, zend_mir_source_edge_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint32_t current = 0;
	uint32_t from;
	if (!zend_mir_source_is_initialized(source) || !source->w04
			|| out == NULL || index >= source->edge_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	for (from = 0; from < ssa->cfg.blocks_count; from++) {
		const zend_basic_block *block = &ssa->cfg.blocks[from];
		uint32_t successor;
		if ((block->flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		for (successor = 0; successor < block->successors_count; successor++) {
			uint32_t to = (uint32_t) block->successors[successor];
			uint32_t predecessor;
			if (current++ != index) {
				continue;
			}
			memset(out, 0, sizeof(*out));
			out->id = index;
			out->from_block_id = from;
			out->to_block_id = to;
			out->successor_index = successor;
			out->predecessor_index = ZEND_MIR_ID_INVALID;
			for (predecessor = 0;
				predecessor < ssa->cfg.blocks[to].predecessors_count;
				predecessor++) {
				uint32_t offset =
					(uint32_t) ssa->cfg.blocks[to].predecessor_offset
						+ predecessor;
				if ((uint32_t) ssa->cfg.predecessors[offset] == from) {
					out->predecessor_index = predecessor;
					break;
				}
			}
			if (out->predecessor_index == ZEND_MIR_ID_INVALID) {
				return false;
			}
			out->flags = successor + 1 == block->successors_count
				? ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
				: ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP;
			if (block->len != 0) {
				uint8_t opcode =
					op_array->opcodes[block->start + block->len - 1].opcode;
				if (opcode == ZEND_JMP || successor == 0) {
					out->flags &= ~ZEND_MIR_SOURCE_EDGE_FALLTHROUGH;
					out->flags |= ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP;
				}
			}
			if (zend_mir_frontend_cfg_dominates(&ssa->cfg, to, from)) {
				out->flags |= ZEND_MIR_SOURCE_EDGE_BACKEDGE
					| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY;
			}
			return true;
		}
	}
	return false;
}

static bool zend_mir_frontend_source_ssa_slot(
	const zend_mir_zend_source *source, uint32_t ssa_variable_id,
	uint32_t *slot, zend_mir_source_slot_kind *slot_kind);

static zend_ssa_phi *zend_mir_frontend_phi_at_index(
	const zend_mir_zend_source *source, uint32_t index,
	uint32_t *block_id_out)
{
	const zend_ssa *ssa = zend_mir_source_ssa(source);
	zend_ssa_phi *selected = NULL;
	uint32_t selected_block = ZEND_MIR_ID_INVALID;
	uint32_t selected_category = ZEND_MIR_ID_INVALID;
	zend_mir_source_slot_kind selected_kind =
		ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	uint32_t selected_slot = ZEND_MIR_ID_INVALID;
	uint32_t selected_result = ZEND_MIR_ID_INVALID;
	uint32_t ordinal;
	for (ordinal = 0; ordinal <= index; ordinal++) {
		zend_ssa_phi *next = NULL;
		uint32_t next_block = ZEND_MIR_ID_INVALID;
		uint32_t next_category = ZEND_MIR_ID_INVALID;
		zend_mir_source_slot_kind next_kind =
			ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		uint32_t next_slot = ZEND_MIR_ID_INVALID;
		uint32_t next_result = ZEND_MIR_ID_INVALID;
		uint32_t block;
		for (block = 0; block < ssa->cfg.blocks_count; block++) {
			zend_ssa_phi *phi;
			if ((ssa->cfg.blocks[block].flags & ZEND_BB_REACHABLE) == 0) {
				continue;
			}
			for (phi = ssa->blocks[block].phis;
					phi != NULL; phi = phi->next) {
				zend_mir_source_slot_kind kind;
				uint32_t slot;
				uint32_t result;
				uint32_t category;
				bool after_selected;
				bool before_next;
				if (phi->ssa_var < 0
						|| !zend_mir_frontend_source_ssa_slot(
							source, (uint32_t) phi->ssa_var,
							&slot, &kind)) {
					return NULL;
				}
				result = (uint32_t) phi->ssa_var;
				category = phi->pi < 0 ? 0 : 1;
				after_selected = selected == NULL
					|| block > selected_block
					|| (block == selected_block
						&& (category > selected_category
							|| (category == selected_category
								&& (kind > selected_kind
									|| (kind == selected_kind
										&& (slot > selected_slot
											|| (slot == selected_slot
												&& result
													> selected_result)))))));
				before_next = next == NULL
					|| block < next_block
					|| (block == next_block
						&& (category < next_category
							|| (category == next_category
								&& (kind < next_kind
									|| (kind == next_kind
										&& (slot < next_slot
											|| (slot == next_slot
												&& result < next_result)))))));
				if (after_selected && before_next) {
					next = phi;
					next_block = block;
					next_category = category;
					next_kind = kind;
					next_slot = slot;
					next_result = result;
				}
			}
		}
		if (next == NULL) {
			return NULL;
		}
		selected = next;
		selected_block = next_block;
		selected_category = next_category;
		selected_kind = next_kind;
		selected_slot = next_slot;
		selected_result = next_result;
	}
	*block_id_out = selected_block;
	return selected;
}

static bool zend_mir_frontend_source_ssa_slot(
	const zend_mir_zend_source *source, uint32_t ssa_variable_id,
	uint32_t *slot, zend_mir_source_slot_kind *slot_kind)
{
	if (zend_mir_frontend_ssa_slot(
			zend_mir_source_op_array(source), zend_mir_source_ssa(source),
			ssa_variable_id, slot, slot_kind)) {
		return true;
	}
	return source != NULL && source->w05
		&& source->call_op_array != NULL && source->call_ssa != NULL
		&& zend_mir_frontend_ssa_slot(
			(const zend_op_array *) source->call_op_array,
			(const zend_ssa *) source->call_ssa,
			ssa_variable_id, slot, slot_kind);
}

static bool zend_mir_frontend_view_phi_at(
	const void *context, uint32_t index, zend_mir_source_phi_ref *out)
{
	const zend_mir_zend_source *source = context;
	zend_ssa_phi *phi;
	uint32_t block;
	if (!zend_mir_source_is_initialized(source) || !source->w04
			|| out == NULL || index >= source->phi_count) {
		return false;
	}
	phi = zend_mir_frontend_phi_at_index(source, index, &block);
	if (phi == NULL || !zend_mir_frontend_source_ssa_slot(source,
			(uint32_t) phi->ssa_var, &out->source_slot_index,
			&out->source_slot_kind)) {
		return false;
	}
	memset(&out->constraint, 0, sizeof(out->constraint));
	out->constraint.min_ssa_variable_id = ZEND_MIR_ID_INVALID;
	out->constraint.max_ssa_variable_id = ZEND_MIR_ID_INVALID;
	out->id = index;
	out->block_id = block;
	out->result_ssa_variable_id = (uint32_t) phi->ssa_var;
	if (phi->pi < 0) {
		out->kind = ZEND_MIR_SOURCE_PHI_MERGE;
	} else if (phi->has_range_constraint) {
		out->kind = ZEND_MIR_SOURCE_PHI_PI_RANGE;
		out->constraint.range_min = (int64_t) phi->constraint.range.range.min;
		out->constraint.range_max = (int64_t) phi->constraint.range.range.max;
		if (phi->constraint.range.min_ssa_var >= 0) {
			out->constraint.min_ssa_variable_id =
				(uint32_t) phi->constraint.range.min_ssa_var;
		}
		if (phi->constraint.range.max_ssa_var >= 0) {
			out->constraint.max_ssa_variable_id =
				(uint32_t) phi->constraint.range.max_ssa_var;
		}
		if (phi->constraint.range.range.underflow) {
			out->constraint.flags |=
				ZEND_MIR_SOURCE_PHI_RANGE_MIN_UNBOUNDED;
		}
		if (phi->constraint.range.range.overflow) {
			out->constraint.flags |=
				ZEND_MIR_SOURCE_PHI_RANGE_MAX_UNBOUNDED;
		}
		if (phi->constraint.range.negative != NEG_NONE) {
			out->constraint.flags |= ZEND_MIR_SOURCE_PHI_RANGE_NEGATED;
		}
	} else {
		out->kind = ZEND_MIR_SOURCE_PHI_PI_TYPE;
		/* Class identity is request-local and must not enter persistent MIR.
		 * Preserve the source-backed object constraint as a portable type mask;
		 * the runtime object operation retains the authoritative class check. */
		out->constraint.type_mask = phi->constraint.type.type_mask != 0
			? phi->constraint.type.type_mask : MAY_BE_OBJECT;
	}
	return true;
}

static bool zend_mir_frontend_view_phi_input_at(
	const void *context, uint32_t index, zend_mir_source_phi_input_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_ssa *ssa;
	uint32_t current = 0;
	uint32_t phi_id;
	if (!zend_mir_source_is_initialized(source) || !source->w04
			|| out == NULL || index >= source->phi_input_count) {
		return false;
	}
	ssa = zend_mir_source_ssa(source);
	for (phi_id = 0; phi_id < source->phi_count; phi_id++) {
		uint32_t block;
		zend_ssa_phi *phi =
			zend_mir_frontend_phi_at_index(source, phi_id, &block);
		if (phi == NULL) {
			return false;
		}
		uint32_t count = phi->pi >= 0
			? 1 : ssa->cfg.blocks[block].predecessors_count;
		uint32_t input;
		for (input = 0; input < count; input++) {
			uint32_t predecessor_index = input;
			if (current++ != index) {
				continue;
			}
			if (phi->sources[input] < 0) {
				return false;
			}
			if (phi->pi >= 0) {
				for (predecessor_index = 0;
					predecessor_index
						< ssa->cfg.blocks[block].predecessors_count;
					predecessor_index++) {
					uint32_t offset =
						(uint32_t) ssa->cfg.blocks[block].predecessor_offset
							+ predecessor_index;
					if (ssa->cfg.predecessors[offset] == phi->pi) {
						break;
					}
				}
				if (predecessor_index
						>= ssa->cfg.blocks[block].predecessors_count) {
					return false;
				}
			}
			out->phi_id = phi_id;
			out->input_index = predecessor_index;
			out->predecessor_block_id = (uint32_t)
				ssa->cfg.predecessors[
					(uint32_t) ssa->cfg.blocks[block].predecessor_offset
						+ predecessor_index];
			out->source_ssa_variable_id = (uint32_t) phi->sources[input];
			return true;
		}
	}
	return false;
}

bool zend_mir_zend_source_view(
	const zend_mir_zend_source *source,
	zend_mir_lowering_source_view *out)
{
	if (!zend_mir_source_is_initialized(source) || out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
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
	out->block_count = zend_mir_frontend_view_block_count;
	out->block_at = zend_mir_frontend_view_block_at;
	out->edge_count = zend_mir_frontend_view_edge_count;
	out->edge_at = zend_mir_frontend_view_edge_at;
	out->phi_count = zend_mir_frontend_view_phi_count;
	out->phi_at = zend_mir_frontend_view_phi_at;
	out->phi_input_count = zend_mir_frontend_view_phi_input_count;
	out->phi_input_at = zend_mir_frontend_view_phi_input_at;
	if (source->w04) {
		out->contract_version = ZEND_MIR_W04_CONTRACT_VERSION;
	}
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
	const zend_mir_frontend_call_inventory *inventory;

	if (zend_mir_frontend_value_fact_at(source, index, out)) {
		return true;
	}
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| source->call_inventory == NULL) {
		return false;
	}
	inventory = source->call_inventory;
	return index >= inventory->base_value_fact_count
		&& zend_mir_frontend_w05_result_fact_at(
			source, index - inventory->base_value_fact_count, out);
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

static bool zend_mir_frontend_function_is_in_script(
	const zend_script *script, const zend_function *function)
{
	zend_function *candidate;

	if (script == NULL || function == NULL) {
		return false;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, candidate) {
		if (candidate == function) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	return false;
}

static zend_function *zend_mir_frontend_canonical_script_function(
	const zend_script *script, zend_function *function)
{
	zend_function *candidate;

	if (script == NULL || function == NULL
			|| function->type != ZEND_USER_FUNCTION
			|| function->common.function_name == NULL) {
		return function;
	}
	candidate = zend_hash_find_ptr(
		&script->function_table, function->common.function_name);
	if (candidate == NULL || candidate->type != ZEND_USER_FUNCTION
			|| candidate->op_array.filename == NULL
			|| function->op_array.filename == NULL
			|| !zend_string_equals(
				candidate->op_array.filename, function->op_array.filename)) {
		return function;
	}
	return candidate;
}

static bool zend_mir_frontend_opline_defines_variable(
	const zend_op_array *op_array, const zend_op *opline, uint32_t variable)
{
	uint32_t variable_count;
	uint32_t set_size;
	bool defines;
	ALLOCA_FLAG(use_heap)
	ALLOCA_FLAG(def_heap)
	zend_bitset use;
	zend_bitset def;

	if (op_array == NULL || opline == NULL
			|| op_array->last_var > UINT32_MAX - op_array->T) {
		return true;
	}
	variable_count = op_array->last_var + op_array->T;
	if (variable >= variable_count) {
		return true;
	}
	set_size = zend_bitset_len(variable_count);
	use = ZEND_BITSET_ALLOCA(set_size, use_heap);
	def = ZEND_BITSET_ALLOCA(set_size, def_heap);
	zend_bitset_clear(use, set_size);
	zend_bitset_clear(def, set_size);
	zend_dfg_add_use_def_op(op_array, opline, 0, use, def);
	defines = zend_bitset_in(def, variable);
	free_alloca(def, def_heap);
	free_alloca(use, use_heap);
	return defines;
}

static zend_class_entry *zend_mir_frontend_catch_receiver_class(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t init_opline_index)
{
	const zend_op *init_opline;
	const zend_op *catch_opline;
	uint32_t block_id;
	uint32_t scan_end;
	uint32_t receiver_variable;
	uint32_t remaining;

	if (op_array == NULL || ssa == NULL || ssa->ops == NULL
			|| ssa->cfg.map == NULL || ssa->cfg.blocks == NULL
			|| init_opline_index >= op_array->last) {
		return NULL;
	}
	init_opline = &op_array->opcodes[init_opline_index];
	if (init_opline->opcode != ZEND_INIT_METHOD_CALL
			|| init_opline->op1_type != IS_CV) {
		return NULL;
	}
	receiver_variable = EX_VAR_TO_NUM(init_opline->op1.var);
	block_id = ssa->cfg.map[init_opline_index];
	if (block_id >= ssa->cfg.blocks_count) {
		return NULL;
	}
	scan_end = init_opline_index;
	remaining = ssa->cfg.blocks_count;
	catch_opline = NULL;
	while (remaining-- != 0) {
		const zend_basic_block *block = &ssa->cfg.blocks[block_id];
		uint32_t index;

		if (block->len == 0 || block->start > op_array->last
				|| block->len > op_array->last - block->start
				|| block->start > scan_end
				|| scan_end > block->start + block->len) {
			return NULL;
		}
		if (op_array->opcodes[block->start].opcode == ZEND_CATCH) {
			catch_opline = &op_array->opcodes[block->start];
			for (index = block->start + 1; index < scan_end; index++) {
				if (zend_mir_frontend_opline_defines_variable(
						op_array, &op_array->opcodes[index],
						receiver_variable)) {
					return NULL;
				}
			}
			break;
		}
		for (index = block->start; index < scan_end; index++) {
			if (zend_mir_frontend_opline_defines_variable(
					op_array, &op_array->opcodes[index], receiver_variable)) {
				return NULL;
			}
		}
		if (block->predecessors_count != 1
				|| ssa->cfg.predecessors == NULL
				|| block->predecessor_offset < 0
				|| (uint32_t) block->predecessor_offset
					>= ssa->cfg.edges_count) {
			return NULL;
		}
		block_id = (uint32_t)
			ssa->cfg.predecessors[block->predecessor_offset];
		if (block_id >= ssa->cfg.blocks_count) {
			return NULL;
		}
		scan_end = ssa->cfg.blocks[block_id].start
			+ ssa->cfg.blocks[block_id].len;
	}
	if (catch_opline == NULL) {
		return NULL;
	}
	if (catch_opline->opcode != ZEND_CATCH
			|| catch_opline->op1_type != IS_CONST
			|| catch_opline->result_type != IS_CV
			|| catch_opline->result.var != init_opline->op1.var
			|| Z_TYPE_P(RT_CONSTANT(catch_opline, catch_opline->op1))
				!= IS_STRING
			|| Z_TYPE_P(RT_CONSTANT(catch_opline, catch_opline->op1) + 1)
				!= IS_STRING) {
		return NULL;
	}
	/* Protected handlers are disconnected from ordinary SSA reachability.
	 * The unique predecessor chain and DFG definition checks prove that this
	 * source catch binding is the value consumed by the method receiver. */
	return zend_fetch_class_by_name(
		Z_STR_P(RT_CONSTANT(catch_opline, catch_opline->op1)),
		Z_STR_P(RT_CONSTANT(catch_opline, catch_opline->op1) + 1),
		ZEND_FETCH_CLASS_NO_AUTOLOAD | ZEND_FETCH_CLASS_SILENT);
}

zend_function *zend_mir_zend_source_resolve_internal_call(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, uint32_t init_opline_index)
{
	const zend_op *opline;
	const zend_ssa_var_info *receiver_info;
	const zend_class_entry *receiver_class;
	zend_function *function;
	bool catch_receiver = false;
	bool is_prototype = false;
	int receiver_ssa;

	if (op_array == NULL || ssa == NULL || ssa->ops == NULL
			|| ssa->var_info == NULL || init_opline_index >= op_array->last) {
		return NULL;
	}
	opline = &op_array->opcodes[init_opline_index];
	function = zend_optimizer_get_called_func(
		script, op_array, (zend_op *) opline, &is_prototype);
	if (function != NULL && !is_prototype
			&& function->type == ZEND_INTERNAL_FUNCTION) {
		return function;
	}
	if (opline->opcode != ZEND_INIT_METHOD_CALL
			|| opline->op1_type == IS_UNUSED
			|| opline->op2_type != IS_CONST
			|| Z_TYPE_P(CRT_CONSTANT(opline->op2)) != IS_STRING
			|| Z_TYPE_P(CRT_CONSTANT(opline->op2) + 1) != IS_STRING) {
		return NULL;
	}
	receiver_ssa = ssa->ops[init_opline_index].op1_use;
	if (receiver_ssa >= 0 && receiver_ssa < ssa->vars_count) {
		receiver_info = &ssa->var_info[receiver_ssa];
		receiver_class = receiver_info->ce;
	} else {
		receiver_info = NULL;
		receiver_class = NULL;
	}
	if (receiver_class == NULL) {
		receiver_class = zend_mir_frontend_catch_receiver_class(
			op_array, ssa, init_opline_index);
		catch_receiver = receiver_class != NULL;
	}
	if (receiver_class == NULL || receiver_class->default_object_handlers == NULL
			|| receiver_class->default_object_handlers->get_method
				!= zend_std_get_method) {
		return NULL;
	}
	function = zend_hash_find_ptr(
		&receiver_class->function_table,
		Z_STR_P(CRT_CONSTANT(opline->op2) + 1));
	if (function == NULL || function->type != ZEND_INTERNAL_FUNCTION
			|| (function->common.fn_flags
				& (ZEND_ACC_ABSTRACT | ZEND_ACC_STATIC | ZEND_ACC_PUBLIC))
				!= ZEND_ACC_PUBLIC) {
		return NULL;
	}
	if ((catch_receiver || receiver_info == NULL
			|| receiver_info->is_instanceof || receiver_info->ce == NULL)
			&& (receiver_class->ce_flags & ZEND_ACC_FINAL) == 0
			&& (function->common.fn_flags & ZEND_ACC_FINAL) == 0) {
		return NULL;
	}
	return function;
}

zend_function *zend_mir_zend_source_resolve_user_method_call(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, uint32_t init_opline_index)
{
	const zend_op *opline;
	const zend_ssa_var_info *receiver_info = NULL;
	const zend_class_entry *receiver_class = NULL;
	zend_function *function;
	bool is_prototype = false;
	int receiver_ssa;

	if (op_array == NULL || ssa == NULL || ssa->ops == NULL
			|| ssa->var_info == NULL || init_opline_index >= op_array->last) {
		return NULL;
	}
	opline = &op_array->opcodes[init_opline_index];
	function = zend_optimizer_get_called_func(
		script, op_array, (zend_op *) opline, &is_prototype);
	if (function != NULL && !is_prototype
			&& function->type == ZEND_USER_FUNCTION
			&& (function->common.fn_flags & ZEND_ACC_ABSTRACT) == 0) {
		return function;
	}
	if (opline->opcode != ZEND_INIT_METHOD_CALL
			|| opline->op2_type != IS_CONST
			|| Z_TYPE_P(CRT_CONSTANT(opline->op2)) != IS_STRING
			|| Z_TYPE_P(CRT_CONSTANT(opline->op2) + 1) != IS_STRING) {
		return NULL;
	}
	if (opline->op1_type == IS_UNUSED) {
		receiver_class = op_array->scope;
	} else {
		int definition;

		receiver_ssa = ssa->ops[init_opline_index].op1_use;
		if (receiver_ssa < 0 || receiver_ssa >= ssa->vars_count) {
			return NULL;
		}
		receiver_info = &ssa->var_info[receiver_ssa];
		receiver_class = receiver_info->ce;
		/* Enum cases are represented as class constants.  The optimizer keeps
		 * their result type deliberately broad, so recover the exact final enum
		 * class from the source definition instead of persisting a class pointer
		 * in MIR or treating the method call as unresolved. */
		definition = ssa->vars[receiver_ssa].definition;
		if (receiver_class == NULL && definition >= 0
				&& (uint32_t) definition < op_array->last
				&& op_array->opcodes[definition].opcode
					== ZEND_FETCH_CLASS_CONSTANT) {
			const zend_class_constant *constant;
			bool constant_is_prototype = false;

			constant = zend_fetch_class_const_info(
				script, op_array, &op_array->opcodes[definition],
				&constant_is_prototype);
			if (constant != NULL && !constant_is_prototype
					&& (ZEND_CLASS_CONST_FLAGS(constant)
						& ZEND_CLASS_CONST_IS_CASE) != 0
					&& constant->ce != NULL
					&& (constant->ce->ce_flags & ZEND_ACC_ENUM) != 0) {
				receiver_class = constant->ce;
			}
		}
	}
	if (receiver_class == NULL
			|| (receiver_class->default_object_handlers != NULL
				&& receiver_class->default_object_handlers->get_method
					!= zend_std_get_method)) {
		return NULL;
	}
	function = zend_hash_find_ptr(
		&receiver_class->function_table,
		Z_STR_P(CRT_CONSTANT(opline->op2) + 1));
	if (function == NULL || function->type != ZEND_USER_FUNCTION
			|| (function->common.fn_flags
				& (ZEND_ACC_ABSTRACT | ZEND_ACC_STATIC)) != 0) {
		return NULL;
	}
	/* The source target is the statically known implementation and supplies
	 * the call signature.  W10 resolves the actual method against the runtime
	 * receiver and may compile an override into a request-local entry cell. */
	return function;
}

bool zend_mir_zend_source_w08_return_source_zval(
	const zend_mir_zend_source *source, uint32_t return_opline_index)
{
	const zend_mir_frontend_call_inventory *inventory;
	const zend_op_array *op_array;
	const zend_op *return_opline;
	uint32_t return_slot;
	zend_mir_source_slot_kind return_slot_kind;
	uint32_t index;

	if (!zend_mir_source_is_initialized(source) || !source->w08 || !source->w05
			|| source->call_op_array == NULL || source->call_inventory == NULL) {
		return false;
	}
	op_array = (const zend_op_array *) source->call_op_array;
	if (return_opline_index >= op_array->last) {
		return false;
	}
	return_opline = &op_array->opcodes[return_opline_index];
	if (return_opline->opcode != ZEND_RETURN
			&& (!source->w09 || return_opline->opcode != ZEND_RETURN_BY_REF)) {
		return false;
	}
	/*
	 * W09 executes zval-producing and aliasing oplines against the canonical
	 * Zend frame.  A scalar SSA value is therefore not an authoritative copy
	 * of a CV/VAR/TMP at return: references, COW separation and destructors may
	 * all have changed the slot.  Transfer the source zval itself.
	 */
	if (source->w09) {
		return return_opline->op1_type == IS_CONST
			|| zend_mir_frontend_decode_slot(
				op_array, &return_opline->op1, return_opline->op1_type,
				&return_slot, &return_slot_kind);
	}
	if (!zend_mir_frontend_decode_slot(
			op_array, &return_opline->op1, return_opline->op1_type,
			&return_slot, &return_slot_kind)) {
		return false;
	}
	inventory = (const zend_mir_frontend_call_inventory *) source->call_inventory;
	for (index = 0; index < inventory->site_count; index++) {
		const zend_mir_source_call_site_ref *site = &inventory->sites[index];
		const zend_mir_frontend_call_target *target;
		const zend_op *do_opline;
		uint32_t result_flags;
		uint32_t result_slot;
		zend_mir_source_slot_kind result_slot_kind;

		if (site->do_opline_index >= return_opline_index
				|| site->do_opline_index >= op_array->last
				|| site->target_id >= inventory->target_count) {
			continue;
		}
		target = &inventory->targets[site->target_id];
		result_flags = site->flags
			& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
				| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
		if (target->record.kind != ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
				|| result_flags != 0
				|| zend_mir_id_is_valid(site->result_ssa_variable_id)) {
			continue;
		}
		do_opline = &op_array->opcodes[site->do_opline_index];
		if (!zend_mir_frontend_decode_slot(
				op_array, &do_opline->result, do_opline->result_type,
				&result_slot, &result_slot_kind)
				|| result_slot != return_slot
				|| result_slot_kind != return_slot_kind) {
			continue;
		}
		/* The returned source slot must not be overwritten after the call. */
		for (uint32_t scan = site->do_opline_index + 1;
				scan < return_opline_index; scan++) {
			uint32_t variable = return_slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				? return_slot
				: return_slot + (uint32_t) op_array->last_var;
			if (zend_mir_frontend_opline_defines_variable(
					op_array, &op_array->opcodes[scan], variable)) {
				goto next_site;
			}
		}
		return true;
	next_site:
		continue;
	}
	return false;
}

static zend_mir_source_call_target_kind
zend_mir_frontend_target_kind(uint8_t opcode, const zend_function *function,
	const zend_script *script, const zend_op_array *caller)
{
	if (opcode == ZEND_INIT_METHOD_CALL
			|| opcode == ZEND_INIT_STATIC_METHOD_CALL
			|| opcode == ZEND_INIT_PARENT_PROPERTY_HOOK_CALL) {
		if (function != NULL && function->type == ZEND_INTERNAL_FUNCTION) {
			return ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL;
		}
		return ZEND_MIR_SOURCE_CALL_TARGET_METHOD;
	}
	if (opcode == ZEND_NEW) {
		return ZEND_MIR_SOURCE_CALL_TARGET_METHOD;
	}
	if (opcode != ZEND_INIT_FCALL) {
		return ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER;
	}
	if (function == NULL || function->type != ZEND_USER_FUNCTION) {
		return function != NULL && function->type == ZEND_INTERNAL_FUNCTION
			? ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
			: ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER;
	}
	if (!zend_mir_frontend_function_is_in_script(script, function)
			|| function->op_array.filename == NULL || caller->filename == NULL
			|| !zend_string_equals(
				function->op_array.filename, caller->filename)) {
		return ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER;
	}
	return ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER;
}

static bool zend_mir_frontend_declaration_id(
	const zend_script *script, const zend_op_array *caller,
	const zend_function *function, uint32_t *id_out)
{
	zend_function *candidate;
	uint32_t declaration_id = 1;

	if (script == NULL || caller == NULL || function == NULL
			|| function->type != ZEND_USER_FUNCTION || id_out == NULL) {
		return false;
	}
	if (&function->op_array == caller) {
		*id_out = 0;
		return true;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, candidate) {
		if (candidate == NULL || candidate->type != ZEND_USER_FUNCTION) {
			continue;
		}
		if (candidate == function) {
			*id_out = declaration_id;
			return true;
		}
		if (declaration_id == ZEND_MIR_ID_MAX) {
			return false;
		}
		declaration_id++;
	} ZEND_HASH_FOREACH_END();
	return false;
}

static bool zend_mir_frontend_append_parameter_modes(
	zend_mir_frontend_call_inventory *inventory,
	zend_mir_source_call_target_ref *target,
	const zend_function *function)
{
	zend_mir_source_parameter_mode_ref *modes;
	uint32_t required;
	uint32_t capacity;
	uint32_t argument;

	target->parameter_modes.offset = inventory->parameter_mode_count;
	target->parameter_modes.count =
		function == NULL ? 0 : function->common.num_args;
	if (target->parameter_modes.count == 0) {
		return true;
	}
	if (inventory->parameter_mode_count
			> UINT32_MAX - target->parameter_modes.count) {
		return false;
	}
	required = inventory->parameter_mode_count + target->parameter_modes.count;
	if (required > inventory->parameter_mode_capacity) {
		capacity = inventory->parameter_mode_capacity == 0
			? UINT32_C(8) : inventory->parameter_mode_capacity;
		while (capacity < required) {
			if (capacity > UINT32_MAX / 2) {
				capacity = required;
				break;
			}
			capacity *= 2;
		}
#if SIZE_MAX <= UINT32_MAX
		if ((size_t) capacity > SIZE_MAX / sizeof(*modes)) {
			return false;
		}
#endif
		modes = realloc(inventory->parameter_modes,
			(size_t) capacity * sizeof(*modes));
		if (modes == NULL) {
			return false;
		}
		inventory->parameter_modes = modes;
		inventory->parameter_mode_capacity = capacity;
	}
	for (argument = 0; argument < target->parameter_modes.count; argument++) {
		zend_mir_source_parameter_mode_ref *mode =
			&inventory->parameter_modes[inventory->parameter_mode_count++];
		mode->target_id = target->id;
		mode->ordinal = argument;
		mode->mode = ARG_MUST_BE_SENT_BY_REF(function, argument + 1)
			? ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
			: ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
	}
	return true;
}

static bool zend_mir_frontend_snapshot_target(
	zend_mir_frontend_call_inventory *inventory,
	zend_mir_frontend_call_target *target,
	zend_mir_source_call_target_id id,
	zend_mir_source_call_target_kind kind,
	const zend_function *function, uint32_t declaration_id,
	bool open_method)
{
	memset(target, 0, sizeof(*target));
	target->record.id = id;
	target->record.kind = kind;
	target->record.function_symbol_id = declaration_id;
	target->record.op_array_id = kind == ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
		? ZEND_MIR_ID_INVALID : declaration_id;
	target->function = function;
	if (kind == ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER || open_method) {
		/* Even when the optimizer can currently see a likely function, PHP's
		 * named, first-class callable, and receiver-polymorphic method forms
		 * resolve against request-local bindings at execution time.  Persist
		 * only the open call-site shape. */
		target->record.variadic = true;
		return zend_mir_frontend_append_parameter_modes(
			inventory, &target->record, NULL);
	}
	if (function == NULL) {
		return zend_mir_frontend_append_parameter_modes(
			inventory, &target->record, function);
	}
	target->record.num_args = function->common.num_args;
	target->record.required_num_args = function->common.required_num_args;
	target->record.function_flags_snapshot = function->common.fn_flags;
	target->record.variadic =
		(function->common.fn_flags & ZEND_ACC_VARIADIC) != 0;
	target->record.returns_by_reference =
		(function->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0;
	return zend_mir_frontend_append_parameter_modes(
		inventory, &target->record, function);
}

static bool zend_mir_frontend_target_for_call(
	zend_mir_frontend_call_inventory *inventory,
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, uint32_t opline_index,
	const zend_op *opline, zend_mir_source_call_target_id *target_id)
{
	zend_function *function = NULL;
	zend_mir_source_call_target_kind kind;
	bool is_prototype = false;
	uint32_t declaration_id;
	uint32_t index;

	if (opline->opcode == ZEND_INIT_FCALL
			|| opline->opcode == ZEND_INIT_FCALL_BY_NAME
			|| opline->opcode == ZEND_INIT_NS_FCALL_BY_NAME
			|| opline->opcode == ZEND_INIT_METHOD_CALL
			|| opline->opcode == ZEND_INIT_STATIC_METHOD_CALL
			|| opline->opcode == ZEND_INIT_PARENT_PROPERTY_HOOK_CALL
			|| opline->opcode == ZEND_NEW) {
		if (opline->opcode != ZEND_INIT_PARENT_PROPERTY_HOOK_CALL) {
			function = zend_optimizer_get_called_func(
				script, op_array, (zend_op *) opline, &is_prototype);
		}
		if (is_prototype) {
			function = NULL;
		}
		if (function == NULL && opline->opcode == ZEND_INIT_METHOD_CALL) {
			function = zend_mir_zend_source_resolve_internal_call(
				script, op_array, ssa, opline_index);
			if (function == NULL) {
				function = zend_mir_zend_source_resolve_user_method_call(
					script, op_array, ssa, opline_index);
			}
		}
		function = zend_mir_frontend_canonical_script_function(
			script, function);
	}
	kind = zend_mir_frontend_target_kind(
		opline->opcode, function, script, op_array);
	for (index = 0; index < inventory->target_count; index++) {
		if (inventory->targets[index].function == function
				&& inventory->targets[index].record.kind == kind
				&& function != NULL) {
			*target_id = index;
			return true;
		}
	}
	if (inventory->target_count == ZEND_MIR_ID_INVALID) {
		return false;
	}
	*target_id = inventory->target_count;
	declaration_id = inventory->target_count + 1;
	if (kind == ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
			&& !zend_mir_frontend_declaration_id(
				script, op_array, function, &declaration_id)) {
		return false;
	}
	if (!zend_mir_frontend_snapshot_target(
		inventory,
		&inventory->targets[inventory->target_count],
		inventory->target_count, kind, function, declaration_id,
		function == NULL && (opline->opcode == ZEND_INIT_METHOD_CALL
			|| opline->opcode == ZEND_INIT_STATIC_METHOD_CALL
			|| opline->opcode
				== ZEND_INIT_PARENT_PROPERTY_HOOK_CALL))) {
		return false;
	}
	inventory->target_count++;
	return true;
}

static bool zend_mir_frontend_reorder_arguments(
	zend_mir_frontend_call_inventory *inventory)
{
	zend_mir_source_call_argument_ref *ordered;
	uint32_t output = 0;
	uint32_t site_id;

	if (inventory->argument_count == 0) {
		for (site_id = 0; site_id < inventory->site_count; site_id++) {
			inventory->sites[site_id].argument_span.offset = 0;
			inventory->sites[site_id].argument_span.count = 0;
		}
		return true;
	}
	ordered = calloc(inventory->argument_count, sizeof(*ordered));
	if (ordered == NULL) {
		return false;
	}
	for (site_id = 0; site_id < inventory->site_count; site_id++) {
		uint32_t ordinal;
		uint32_t matched = 0;
		uint32_t index;

		inventory->sites[site_id].argument_span.offset = output;
		for (ordinal = 0; ordinal < inventory->argument_count; ordinal++) {
			for (index = 0; index < inventory->argument_count; index++) {
				const zend_mir_source_call_argument_ref *argument =
					&inventory->arguments[index];

				if (argument->call_site_id == site_id
						&& argument->ordinal == ordinal) {
					ordered[output] = *argument;
					ordered[output].id = output;
					output++;
					matched++;
					break;
				}
			}
		}
		inventory->sites[site_id].argument_span.count = matched;
	}
	free(inventory->arguments);
	inventory->arguments = ordered;
	return output == inventory->argument_count;
}

static bool zend_mir_frontend_call_result_is_scalar(
	const zend_op_array *op_array, const zend_ssa *ssa, uint32_t opline_index,
	uint32_t ssa_variable_id)
{
	zend_mir_value_fact_ref fact;

	return zend_mir_frontend_fact_for_ssa(
			op_array, ssa, ssa_variable_id, &fact)
		&& zend_mir_scalar_type_is_exact(fact.exact_type)
		&& (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0
		&& ssa->ops[opline_index].result_def == (int) ssa_variable_id;
}

static zend_mir_lowering_status zend_mir_frontend_build_call_inventory(
	zend_mir_zend_source *source, const zend_script *script,
	const zend_op_array *op_array, const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_frontend_call_inventory *inventory;
	zend_mir_source_call_site_id *stack;
	uint32_t stack_count = 0;
	uint32_t index;

	if (op_array->last != 0
			&& (op_array->opcodes == NULL || ssa->ops == NULL
				|| ssa->cfg.map == NULL)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	inventory = calloc(1, sizeof(*inventory));
	stack = calloc(op_array->last == 0 ? 1 : op_array->last, sizeof(*stack));
	if (inventory == NULL || stack == NULL) {
		free(stack);
		zend_mir_frontend_release_call_inventory(inventory);
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_W05_CALL_PLAN_FAILED, source->op_array_id,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_FAILED;
	}
	inventory->sites = calloc(
		op_array->last == 0 ? 1 : op_array->last,
		sizeof(*inventory->sites));
	inventory->targets = calloc(
		op_array->last == 0 ? 1 : op_array->last,
		sizeof(*inventory->targets));
	inventory->arguments = calloc(
		op_array->last == 0 ? 1 : op_array->last,
		sizeof(*inventory->arguments));
	if (inventory->sites == NULL || inventory->targets == NULL
			|| inventory->arguments == NULL) {
		free(stack);
		zend_mir_frontend_release_call_inventory(inventory);
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_W05_CALL_PLAN_FAILED, source->op_array_id,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_FAILED;
	}

	for (index = 0; index < op_array->last; index++) {
		const zend_op *opline = &op_array->opcodes[index];
		uint32_t block_id = ssa->cfg.map[index];
		if (block_id >= ssa->cfg.blocks_count
				|| (ssa->cfg.blocks[block_id].flags
					& ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		if (zend_mir_frontend_is_call_init(opline->opcode)) {
			zend_mir_source_call_site_ref *site;
			zend_mir_source_call_target_id target_id;

			if (inventory->site_count >= op_array->last
					|| !zend_mir_frontend_target_for_call(
						inventory, script, op_array, ssa, index,
						opline, &target_id)) {
				goto allocation_failed;
			}
			site = &inventory->sites[inventory->site_count];
			memset(site, 0, sizeof(*site));
			site->id = inventory->site_count;
			site->parent_call_site_id = stack_count == 0
				? ZEND_MIR_ID_INVALID : stack[stack_count - 1];
			site->init_opline_index = index;
			site->do_opline_index = ZEND_MIR_ID_INVALID;
			site->source_block_id = block_id;
			site->target_id = target_id;
			site->result_ssa_variable_id = ZEND_MIR_ID_INVALID;
			site->result_operand.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
			site->result_operand.slot_kind =
				ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
			site->result_operand.index = ZEND_MIR_ID_INVALID;
			site->result_operand.ssa_variable_id = ZEND_MIR_ID_INVALID;
			if (stack_count != 0) {
				site->flags |= ZEND_MIR_SOURCE_CALL_SITE_NESTED;
			}
			if ((ssa->cfg.blocks[block_id].flags
					& ZEND_BB_PROTECTED) != 0) {
				zend_mir_source_block_id handler_block;
				uint32_t handler_opline;

				/*
				 * ZEND_BB_PROTECTED also covers the body of a terminal finally.
				 * Such a call has no in-frame exception destination: it must
				 * propagate to the caller. Mark only calls with a real, stable
				 * catch/finally destination as protected call sites.
				 */
				if (zend_mir_zend_source_exception_handler(
						source, index, &handler_block, &handler_opline)) {
					site->flags |= ZEND_MIR_SOURCE_CALL_SITE_PROTECTED;
				}
			}
			stack[stack_count++] = inventory->site_count++;
			continue;
		}
		if (zend_mir_frontend_is_call_send(opline->opcode)) {
			zend_mir_source_call_argument_ref *argument;
			zend_mir_source_opcode_ref source_opcode;
			zend_mir_zend_source original_source;
			zend_mir_source_call_site_id call_site_id;
			uint32_t next_ordinal = 0;
			uint32_t argument_index;

			if (stack_count == 0) {
				zend_mir_frontend_set_diagnostic(
					diagnostic, ZEND_MIR_LOWERING_REJECTED,
					ZEND_MIRL_W05_ORPHAN_CALL_FRAGMENT,
					source->op_array_id, index,
					ZEND_MIR_FRONTEND_OPERAND_NONE,
					ZEND_MIR_ID_INVALID);
				goto rejected;
			}
			argument = &inventory->arguments[inventory->argument_count];
			memset(argument, 0, sizeof(*argument));
			argument->id = inventory->argument_count;
			call_site_id = stack[stack_count - 1];
			argument->call_site_id = call_site_id;
			argument->send_opline_index = index;
			for (argument_index = 0;
					argument_index < inventory->argument_count;
					argument_index++) {
				if (inventory->arguments[argument_index].call_site_id
						== call_site_id) {
					next_ordinal++;
				}
			}
			argument->mode =
				zend_mir_frontend_call_argument_mode(opline);
			argument->ordinal = argument->mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED
				|| argument->mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK
				|| argument->mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
				? next_ordinal
				: (opline->op2.num == 0
					? next_ordinal : opline->op2.num - 1);
			argument->name_symbol_id = ZEND_MIR_ID_INVALID;
			argument->flags = 0;
			argument->value_ssa_variable_id =
				ssa->ops[index].op1_use >= 0
					? (uint32_t) ssa->ops[index].op1_use
					: ZEND_MIR_ID_INVALID;
			original_source = *source;
			original_source.op_array = op_array;
			original_source.ssa = ssa;
			original_source.opcode_count = op_array->last;
			if (!zend_mir_frontend_opcode_at(
					&original_source, index, &source_opcode)) {
				goto malformed;
			}
			argument->source_operand = source_opcode.op1;
			inventory->argument_count++;
			continue;
		}
		if (zend_mir_frontend_is_call_do(opline->opcode)) {
			zend_mir_source_call_site_ref *site;
			zend_mir_source_opcode_ref source_opcode;
			zend_mir_zend_source original_source;

			if (stack_count == 0) {
				zend_mir_frontend_set_diagnostic(
					diagnostic, ZEND_MIR_LOWERING_REJECTED,
					ZEND_MIRL_W05_ORPHAN_CALL_FRAGMENT,
					source->op_array_id, index,
					ZEND_MIR_FRONTEND_OPERAND_NONE,
					ZEND_MIR_ID_INVALID);
				goto rejected;
			}
			site = &inventory->sites[stack[--stack_count]];
			site->do_opline_index = index;
			site->source_block_id = block_id;
			original_source = *source;
			original_source.op_array = op_array;
			original_source.ssa = ssa;
			original_source.opcode_count = op_array->last;
			if (!zend_mir_frontend_opcode_at(
					&original_source, index, &source_opcode)) {
				goto malformed;
			}
			site->result_operand = source_opcode.result;
			if (RESULT_UNUSED(opline)) {
				site->flags |= ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED;
			} else if (ssa->ops[index].result_def >= 0
					&& site->target_id < inventory->target_count
					&& (inventory->targets[site->target_id].record.kind
							== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
						|| zend_mir_frontend_call_result_is_scalar(
							op_array, ssa, index,
							(uint32_t) ssa->ops[index].result_def))) {
				site->flags |= ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR;
				site->result_ssa_variable_id =
					(uint32_t) ssa->ops[index].result_def;
			}
		}
	}
	if (stack_count != 0) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE, source->op_array_id,
			inventory->sites[stack[stack_count - 1]].init_opline_index,
			ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
		goto rejected;
	}
	if (!zend_mir_frontend_reorder_arguments(inventory)) {
		goto malformed;
	}
	free(stack);
	source->call_inventory = inventory;
	source->call_op_array = op_array;
	source->call_ssa = ssa;
	source->script = script;
	source->call_site_count = inventory->site_count;
	source->call_target_count = inventory->target_count;
	source->call_argument_count = inventory->argument_count;
	source->call_parameter_mode_count = inventory->parameter_mode_count;
	source->w05 = true;
	return ZEND_MIR_LOWERING_SUCCESS;

malformed:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED,
		ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE, source->op_array_id,
		index, ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
rejected:
	free(stack);
	zend_mir_frontend_release_call_inventory(inventory);
	return ZEND_MIR_LOWERING_REJECTED;

allocation_failed:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_FAILED,
		ZEND_MIRL_W05_CALL_PLAN_FAILED, source->op_array_id,
		index, ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
	free(stack);
	zend_mir_frontend_release_call_inventory(inventory);
	return ZEND_MIR_LOWERING_FAILED;
}

static bool zend_mir_frontend_w05_argument_is_scalar(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_source_call_argument_ref *argument)
{
	zend_mir_value_fact_ref fact;
	zend_mir_source_literal_ref literal;

	if (argument->value_ssa_variable_id != ZEND_MIR_ID_INVALID) {
		return zend_mir_frontend_fact_for_ssa(
				op_array, ssa, argument->value_ssa_variable_id, &fact)
			&& zend_mir_scalar_type_is_exact(fact.exact_type)
			&& (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0;
	}
	return argument->source_operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
		&& zend_mir_frontend_canonical_literal_for_index(
			op_array, argument->source_operand.index, &literal);
}

static bool zend_mir_frontend_w05_argument_is_call_result(
	const zend_mir_frontend_call_inventory *inventory,
	const zend_mir_source_call_argument_ref *argument)
{
	uint32_t index;

	if (!zend_mir_id_is_valid(argument->value_ssa_variable_id)) {
		return false;
	}
	for (index = 0; index < inventory->site_count; index++) {
		if (inventory->sites[index].result_ssa_variable_id
				== argument->value_ssa_variable_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_frontend_w05_result_is_supported(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_frontend_call_inventory *inventory,
	const zend_mir_source_call_site_ref *site);

static uint32_t zend_mir_frontend_w05_return_type_mask(
	const zend_function *function)
{
	uint32_t type;

	if (function == NULL
			|| (function->type != ZEND_USER_FUNCTION
				&& function->type != ZEND_INTERNAL_FUNCTION)
			|| function->common.arg_info == NULL
			|| (function->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) == 0
			|| (function->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0) {
		return 0;
	}
	type = ZEND_TYPE_PURE_MASK(function->common.arg_info[-1].type);
	switch (type) {
		case MAY_BE_NULL:
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
		case MAY_BE_LONG:
		case MAY_BE_DOUBLE:
			return type;
		default:
			return 0;
	}
}

bool zend_mir_zend_source_w06_call_return_type(
	const zend_mir_zend_source *source,
	zend_mir_source_call_target_id target_id,
	uint32_t *type_mask)
{
	const zend_mir_frontend_call_inventory *inventory;
	const zend_mir_frontend_call_target *target;
	uint32_t type;

	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| type_mask == NULL || target_id >= source->call_target_count
			|| source->call_inventory == NULL) {
		return false;
	}
	inventory = source->call_inventory;
	target = &inventory->targets[target_id];
	if (target->record.kind != ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
			|| target->function == NULL
			|| target->function->type != ZEND_USER_FUNCTION
			|| target->function->common.arg_info == NULL
			|| (target->function->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) == 0) {
		return false;
	}
	type = ZEND_TYPE_PURE_MASK(target->function->common.arg_info[-1].type);
	switch (type) {
		case MAY_BE_NULL:
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
		case MAY_BE_LONG:
		case MAY_BE_DOUBLE:
		case MAY_BE_STRING:
			*type_mask = type;
			return true;
		default:
			return false;
	}
}

static zend_mir_scalar_type_mask zend_mir_frontend_w05_return_scalar_type(
	const zend_function *function)
{
	switch (zend_mir_frontend_w05_return_type_mask(function)) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static zend_mir_lowering_status zend_mir_frontend_project_call_result_facts(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic,
	bool source_backed_internal_results,
	bool source_backed_all_results,
	bool source_backed_method_results)
{
	zend_mir_zend_source source;
	zend_mir_frontend_call_inventory *inventory;
	zend_mir_lowering_status status;
	uint32_t index;

	if (script == NULL || op_array == NULL || ssa == NULL
			|| projected_ssa == NULL
			|| projected_ssa->vars_count != ssa->vars_count
			|| (projected_ssa->vars_count != 0
				&& (projected_ssa->vars == NULL
					|| projected_ssa->var_info == NULL))) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE, ZEND_MIR_ID_INVALID,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	memset(&source, 0, sizeof(source));
	source.op_array = op_array;
	source.ssa = ssa;
	source.op_array_id = 0;
	source.file_symbol_id = 0;
	source.opcode_count = op_array->last;
	source.w04 = true;
	source.initialized = ZEND_MIR_ZEND_SOURCE_MAGIC;
	status = zend_mir_frontend_build_call_inventory(
		&source, script, op_array, ssa, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	inventory = source.call_inventory;
	for (index = 0; index < inventory->site_count; index++) {
		const zend_mir_source_call_site_ref *site = &inventory->sites[index];
		const zend_mir_frontend_call_target *target;
		zend_ssa_var *variable;
		zend_ssa_var_info *info;
		int result_def;
		uint32_t type;

		if ((site->flags & ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED) != 0) {
			continue;
		}
		if (site->target_id >= inventory->target_count
				|| site->do_opline_index >= op_array->last) {
			goto unsupported_result;
		}
		target = &inventory->targets[site->target_id];
		if ((target->record.kind != ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				&& target->record.kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
				&& (!source_backed_all_results
					|| target->record.kind
						!= ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER)
				&& (!source_backed_method_results
					|| target->record.kind
						!= ZEND_MIR_SOURCE_CALL_TARGET_METHOD))
				|| (target->record.returns_by_reference
					&& !source_backed_all_results)) {
			goto unsupported_result;
		}
		result_def = ssa->ops[site->do_opline_index].result_def;
		if (result_def < 0 || result_def >= projected_ssa->vars_count) {
			if (source_backed_all_results
					|| (source_backed_internal_results
					&& target->record.kind
						== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL)) {
				continue;
			}
			goto unsupported_result;
		}
		type = zend_mir_frontend_w05_return_type_mask(target->function);
		variable = &projected_ssa->vars[result_def];
		info = &projected_ssa->var_info[result_def];
		/* W09 executes call results as canonical zvals.  Do not force exact
		 * scalar facts onto values that may carry references, refcounted
		 * payloads, or opaque object/resource identity. */
		if (source_backed_all_results) {
			info->has_range = 0;
			continue;
		}
		/* General W08 internal results remain in their source zval slot. */
		if (source_backed_internal_results
				&& target->record.kind
					== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
				&& (type == 0 || variable->alias != NO_ALIAS
					|| info->guarded_reference || info->indirect_reference
					|| info->ce != NULL || info->is_instanceof
					|| !zend_mir_scalar_type_is_exact(
						zend_mir_frontend_w05_return_scalar_type(
							target->function)))) {
			info->has_range = 0;
			continue;
		}
		if (type == 0 || variable->alias != NO_ALIAS
				|| info->guarded_reference || info->indirect_reference
				|| info->ce != NULL || info->is_instanceof) {
			goto unsupported_result;
		}
		info->type = type;
		info->has_range = 0;
	}
	zend_mir_zend_source_release_w05(&source);
	return ZEND_MIR_LOWERING_SUCCESS;

unsupported_result:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_DEFERRED,
			ZEND_MIRL_W05_UNSUPPORTED_RESULT, 0,
			inventory->sites[index].do_opline_index,
			ZEND_MIR_FRONTEND_RESULT,
			inventory->sites[index].do_opline_index < op_array->last
				&& ssa->ops[inventory->sites[index].do_opline_index]
					.result_def >= 0
			? (uint32_t) ssa->ops[
				inventory->sites[index].do_opline_index].result_def
			: ZEND_MIR_ID_INVALID);
	zend_mir_zend_source_release_w05(&source);
	return ZEND_MIR_LOWERING_DEFERRED;
}

zend_mir_lowering_status zend_mir_frontend_project_w05_result_facts(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_project_call_result_facts(
		script, op_array, ssa, projected_ssa, diagnostic,
		false, false, false);
}

zend_mir_lowering_status zend_mir_frontend_project_w08_result_facts(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_project_call_result_facts(
		script, op_array, ssa, projected_ssa, diagnostic,
		true, false, false);
}

zend_mir_lowering_status zend_mir_frontend_project_w09_result_facts(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_project_call_result_facts(
		script, op_array, ssa, projected_ssa, diagnostic,
		true, true, false);
}

zend_mir_lowering_status zend_mir_frontend_project_w10_result_facts(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_project_call_result_facts(
		script, op_array, ssa, projected_ssa, diagnostic,
		true, true, true);
}

static bool zend_mir_frontend_w05_declared_result_fact(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_frontend_call_inventory *inventory,
	const zend_mir_source_call_site_ref *site, uint32_t fact_id,
	zend_mir_value_fact_ref *out)
{
	zend_mir_value_fact_ref fact;
	const zend_mir_frontend_call_target *target;
	zend_mir_scalar_type_mask exact_type;

	if (op_array == NULL || ssa == NULL || inventory == NULL || site == NULL
			|| out == NULL || site->target_id >= inventory->target_count
			|| !zend_mir_id_is_valid(site->result_ssa_variable_id)
			|| site->result_ssa_variable_id >= (uint32_t) ssa->vars_count
			|| site->do_opline_index >= op_array->last
			|| ssa->ops[site->do_opline_index].result_def
				!= (int) site->result_ssa_variable_id
			|| zend_mir_frontend_fact_for_ssa(
				op_array, ssa, site->result_ssa_variable_id, &fact)) {
		return false;
	}
	target = &inventory->targets[site->target_id];
	if ((target->record.kind != ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
			&& target->record.kind != ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL)
			|| target->record.returns_by_reference) {
		return false;
	}
	exact_type = zend_mir_frontend_w05_return_scalar_type(target->function);
	if (!zend_mir_scalar_type_is_exact(exact_type)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->id = fact_id;
	out->value_id =
		zend_mir_value_from_original_ssa(site->result_ssa_variable_id);
	out->exact_type = exact_type;
	out->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	out->provenance = ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS;
	out->provenance_source_position_id = site->do_opline_index;
	return true;
}

static bool zend_mir_frontend_w05_result_is_supported(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_frontend_call_inventory *inventory,
	const zend_mir_source_call_site_ref *site)
{
	zend_mir_value_fact_ref fact;
	uint32_t result_flags = site->flags
		& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
			| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);

	if (result_flags == ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED) {
		return !zend_mir_id_is_valid(site->result_ssa_variable_id);
	}
	if (result_flags != ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
			|| !zend_mir_id_is_valid(site->result_ssa_variable_id)) {
		return false;
	}
	return (zend_mir_frontend_fact_for_ssa(
				op_array, ssa, site->result_ssa_variable_id, &fact)
			&& zend_mir_scalar_type_is_exact(fact.exact_type)
			&& (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0)
		|| zend_mir_frontend_w05_declared_result_fact(
			op_array, ssa, inventory, site, 0, &fact);
}

bool zend_mir_frontend_w05_result_fact_at(
	const zend_mir_zend_source *source, uint32_t index,
	zend_mir_value_fact_ref *out)
{
	const zend_mir_frontend_call_inventory *inventory;
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint32_t site_index;
	uint32_t current = 0;

	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| source->call_inventory == NULL || out == NULL) {
		return false;
	}
	inventory = source->call_inventory;
	if (index >= inventory->result_fact_count) {
		return false;
	}
	op_array = source->call_op_array;
	ssa = source->call_ssa;
	for (site_index = 0; site_index < inventory->site_count; site_index++) {
		if (zend_mir_frontend_w05_declared_result_fact(
				op_array, ssa, inventory, &inventory->sites[site_index],
				inventory->base_value_fact_count + current, out)) {
			if (current == index) {
				return true;
			}
			current++;
		}
	}
	return false;
}

static zend_mir_lowering_status zend_mir_zend_source_preflight_direct_calls(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic,
	bool w07_execution, bool w08_execution, bool w09_execution,
	bool w10_execution,
	bool allow_empty_calls)
{
	zend_mir_zend_source source;
	zend_mir_frontend_call_inventory *inventory;
	zend_mir_lowering_status status;
	uint32_t index;

	if (script == NULL || op_array == NULL || ssa == NULL
			|| ssa->cfg.blocks == NULL) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE, ZEND_MIR_ID_INVALID,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	memset(&source, 0, sizeof(source));
	source.op_array = op_array;
	source.ssa = ssa;
	source.op_array_id = 0;
	source.file_symbol_id = 0;
	source.opcode_count = op_array->last;
	source.w04 = true;
	source.initialized = ZEND_MIR_ZEND_SOURCE_MAGIC;
	status = zend_mir_frontend_build_call_inventory(
		&source, script, op_array, ssa, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	inventory = source.call_inventory;
	if ((inventory->site_count == 0 || inventory->target_count == 0)
			&& !allow_empty_calls) {
		zend_mir_zend_source_release_w05(&source);
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_DEFERRED,
			ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED, 0,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	for (index = 0; index < inventory->argument_count; index++) {
		const zend_mir_source_call_argument_ref *argument =
			&inventory->arguments[index];
		if (!w07_execution && !w08_execution
				&& zend_mir_frontend_w05_argument_is_call_result(
					inventory, argument)) {
			uint32_t opline_index = argument->send_opline_index;
			uint32_t ssa_variable_id = argument->value_ssa_variable_id;
			zend_mir_zend_source_release_w05(&source);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED,
				ZEND_MIRL_W05_UNSUPPORTED_RESULT, 0, opline_index,
				ZEND_MIR_FRONTEND_OP1, ssa_variable_id);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if ((w09_execution
				? (argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					&& argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
					&& argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED
					&& argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK
					&& (!w10_execution || argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER))
			: w08_execution
				? (argument->mode != ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					&& argument->mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE)
				: argument->mode
					!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE)
				|| argument->flags != 0
				|| zend_mir_id_is_valid(argument->name_symbol_id)
				|| (argument->mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
					? (!w10_execution || argument->source_operand.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED)
					: ((w08_execution || w09_execution)
					? (argument->source_operand.kind
							< ZEND_MIR_SOURCE_OPERAND_LITERAL
						|| argument->source_operand.kind
							> ZEND_MIR_SOURCE_OPERAND_SSA)
					: !zend_mir_frontend_w05_argument_is_scalar(
						op_array, ssa, argument)))) {
			uint32_t opline_index = argument->send_opline_index;
			uint32_t ssa_variable_id = argument->value_ssa_variable_id;
			zend_mir_zend_source_release_w05(&source);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED,
				ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT, 0, opline_index,
				ZEND_MIR_FRONTEND_OP1, ssa_variable_id);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	}
	for (index = 0; index < inventory->target_count; index++) {
		const zend_mir_source_call_target_ref *target =
			&inventory->targets[index].record;
		if ((!w08_execution
				&& target->kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER)
				|| (w08_execution
					&& target->kind
						!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
					&& target->kind
							!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
					&& (!w10_execution || target->kind
							!= ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER)
					&& (!w10_execution || target->kind
							!= ZEND_MIR_SOURCE_CALL_TARGET_METHOD))
				|| (!w09_execution
					&& target->kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
					&& (target->variadic || target->returns_by_reference))) {
			zend_mir_zend_source_release_w05(&source);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED,
				ZEND_MIRL_W05_UNSUPPORTED_TARGET, 0,
				ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
				ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (target->parameter_modes.offset
				> inventory->parameter_mode_count
				|| target->parameter_modes.count
					> inventory->parameter_mode_count
						- target->parameter_modes.offset
				|| target->parameter_modes.count != target->num_args) {
			zend_mir_zend_source_release_w05(&source);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_W05_CALL_PLAN_FAILED, 0,
				ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
				ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_FAILED;
		}
		{
			uint32_t mode_index;
			for (mode_index = 0;
					mode_index < target->parameter_modes.count;
					mode_index++) {
				const zend_mir_source_parameter_mode_ref *mode =
					&inventory->parameter_modes[
						target->parameter_modes.offset + mode_index];
				if (mode->target_id != target->id
						|| mode->ordinal != mode_index) {
					zend_mir_zend_source_release_w05(&source);
					zend_mir_frontend_set_diagnostic(
						diagnostic, ZEND_MIR_LOWERING_FAILED,
						ZEND_MIRL_W05_CALL_PLAN_FAILED, 0,
						ZEND_MIR_ID_INVALID,
						ZEND_MIR_FRONTEND_OPERAND_NONE,
						ZEND_MIR_ID_INVALID);
					return ZEND_MIR_LOWERING_FAILED;
				}
				if (!w08_execution
						&& mode->mode
							!= ZEND_MIR_SOURCE_PARAMETER_BY_VALUE) {
					zend_mir_zend_source_release_w05(&source);
					zend_mir_frontend_set_diagnostic(
						diagnostic, ZEND_MIR_LOWERING_DEFERRED,
						ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT, 0,
						ZEND_MIR_ID_INVALID,
						ZEND_MIR_FRONTEND_OPERAND_NONE,
						ZEND_MIR_ID_INVALID);
					return ZEND_MIR_LOWERING_DEFERRED;
				}
			}
		}
	}
	for (index = 0; index < inventory->site_count; index++) {
		const zend_mir_source_call_site_ref *site = &inventory->sites[index];
		const zend_mir_source_call_target_ref *target;
		zend_mir_lowering_diagnostic_code code = ZEND_MIRL_OK;

		if (site->init_opline_index >= op_array->last
				|| site->do_opline_index >= op_array->last) {
			code = ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
		} else if (site->target_id >= inventory->target_count) {
			code = ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
		} else {
			uint8_t init_opcode =
				op_array->opcodes[site->init_opline_index].opcode;
			uint8_t do_opcode =
				op_array->opcodes[site->do_opline_index].opcode;
			target = &inventory->targets[site->target_id].record;
			if ((target->kind == ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
					&& (!w08_execution
						|| (init_opcode != ZEND_INIT_FCALL
							&& init_opcode != ZEND_INIT_METHOD_CALL
							&& init_opcode != ZEND_INIT_STATIC_METHOD_CALL
							&& (!w10_execution || init_opcode
								!= ZEND_INIT_PARENT_PROPERTY_HOOK_CALL))
							|| (do_opcode != ZEND_DO_ICALL
								&& do_opcode != ZEND_DO_FCALL
								&& (!w10_execution
									|| (do_opcode != ZEND_CALLABLE_CONVERT
										&& do_opcode
											!= ZEND_CALLABLE_CONVERT_PARTIAL)))))
					|| (target->kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
						&& (init_opcode != ZEND_INIT_FCALL
							|| (do_opcode != ZEND_DO_UCALL
								&& do_opcode != ZEND_DO_FCALL
								&& (!w10_execution
									|| (do_opcode != ZEND_CALLABLE_CONVERT
										&& do_opcode
											!= ZEND_CALLABLE_CONVERT_PARTIAL)))))
					|| (target->kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER
						&& (!w10_execution
							|| (init_opcode != ZEND_INIT_FCALL
								&& init_opcode != ZEND_INIT_FCALL_BY_NAME
								&& init_opcode != ZEND_INIT_NS_FCALL_BY_NAME
								&& init_opcode != ZEND_INIT_DYNAMIC_CALL
								&& init_opcode != ZEND_INIT_USER_CALL)
							|| (do_opcode != ZEND_DO_UCALL
								&& do_opcode != ZEND_DO_FCALL
								&& do_opcode != ZEND_DO_FCALL_BY_NAME
								&& do_opcode != ZEND_DO_ICALL
								&& do_opcode != ZEND_CALLABLE_CONVERT
								&& do_opcode
									!= ZEND_CALLABLE_CONVERT_PARTIAL)))
					|| (target->kind == ZEND_MIR_SOURCE_CALL_TARGET_METHOD
						&& (!w10_execution
							|| (init_opcode != ZEND_INIT_METHOD_CALL
								&& init_opcode != ZEND_INIT_STATIC_METHOD_CALL
								&& init_opcode
									!= ZEND_INIT_PARENT_PROPERTY_HOOK_CALL
								&& init_opcode != ZEND_NEW)
							|| (do_opcode != ZEND_DO_UCALL
								&& do_opcode != ZEND_DO_FCALL
								&& do_opcode != ZEND_CALLABLE_CONVERT
								&& do_opcode
									!= ZEND_CALLABLE_CONVERT_PARTIAL)))) {
				code = ZEND_MIRL_W05_UNSUPPORTED_TARGET;
			} else if (!w08_execution
					&& (site->flags
						& ZEND_MIR_SOURCE_CALL_SITE_PROTECTED) != 0) {
				code = ZEND_MIRL_W05_PROTECTED_CALL;
			} else if (!w07_execution && !w08_execution && (site->flags
					& (ZEND_MIR_SOURCE_CALL_SITE_NESTED
						| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR))
					== (ZEND_MIR_SOURCE_CALL_SITE_NESTED
						| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR)) {
				code = ZEND_MIRL_W05_UNSUPPORTED_RESULT;
			} else if (!w09_execution && target->kind
					== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
					&& !zend_mir_frontend_w05_result_is_supported(
						op_array, ssa, inventory, site)) {
				code = ZEND_MIRL_W05_UNSUPPORTED_RESULT;
			} else if (!w09_execution
					&& ((!w07_execution && !w08_execution
					&& site->argument_span.count != target->num_args)
					|| ((w07_execution || w08_execution)
						&& (site->argument_span.count
							< target->required_num_args
							|| (!target->variadic
								&& site->argument_span.count
									> target->num_args))))) {
				code = ZEND_MIRL_W05_ARGUMENT_COUNT_MISMATCH;
			}
		}
		if (code != ZEND_MIRL_OK) {
			uint32_t opline_index = site->do_opline_index;
			zend_mir_zend_source_release_w05(&source);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED, code, 0,
				opline_index, ZEND_MIR_FRONTEND_OPERAND_NONE,
				ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	}
	zend_mir_zend_source_release_w05(&source);
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_zend_source_preflight_w05(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_preflight_direct_calls(
		script, op_array, ssa, diagnostic, false, false, false, false, false);
}

zend_mir_lowering_status zend_mir_zend_source_preflight_w07(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_preflight_direct_calls(
		script, op_array, ssa, diagnostic, true, false, false, false, false);
}

zend_mir_lowering_status zend_mir_zend_source_preflight_w08(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_preflight_direct_calls(
		script, op_array, ssa, diagnostic, true, true, false, false, false);
}

zend_mir_lowering_status zend_mir_zend_source_preflight_w09(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_preflight_direct_calls(
		script, op_array, ssa, diagnostic, true, true, true, false, true);
}

zend_mir_lowering_status zend_mir_zend_source_preflight_w10(
	const zend_script *script, const zend_op_array *op_array,
	const zend_ssa *ssa, zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_zend_source_preflight_direct_calls(
		script, op_array, ssa, diagnostic, true, true, true, true, true);
}

zend_mir_lowering_status zend_mir_zend_source_enable_w05(
	zend_mir_zend_source *source, const zend_script *script,
	const zend_op_array *op_array, const zend_ssa *ssa,
	zend_mir_frontend_diagnostic *diagnostic)
{
	zend_mir_frontend_call_inventory *inventory;
	zend_mir_lowering_status status;
	zend_mir_value_fact_ref fact;
	uint32_t index;

	if (!zend_mir_source_is_initialized(source) || !source->w04
			|| source->w05 || script == NULL || op_array == NULL || ssa == NULL
			|| ssa->cfg.blocks == NULL) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE,
			source != NULL ? source->op_array_id : ZEND_MIR_ID_INVALID,
			ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
			ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	status = zend_mir_frontend_build_call_inventory(
		source, script, op_array, ssa, diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	inventory = source->call_inventory;
	inventory->base_value_fact_count = source->value_fact_count;
	for (index = 0; index < inventory->site_count; index++) {
		if (zend_mir_id_is_valid(
				inventory->sites[index].result_ssa_variable_id)
				&& zend_mir_frontend_fact_for_ssa(
					zend_mir_source_op_array(source),
					zend_mir_source_ssa(source),
					inventory->sites[index].result_ssa_variable_id,
					&fact)) {
			continue;
		}
		if (zend_mir_frontend_w05_declared_result_fact(
				op_array, ssa, inventory, &inventory->sites[index],
				inventory->base_value_fact_count
					+ inventory->result_fact_count,
				&fact)) {
			if (source->value_fact_count == ZEND_MIR_ID_MAX) {
				zend_mir_zend_source_release_w05(source);
				zend_mir_frontend_set_diagnostic(
					diagnostic, ZEND_MIR_LOWERING_FAILED,
					ZEND_MIRL_W05_CALL_PLAN_FAILED, source->op_array_id,
					ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
					ZEND_MIR_ID_INVALID);
				return ZEND_MIR_LOWERING_FAILED;
			}
			source->value_fact_count++;
			inventory->result_fact_count++;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

void zend_mir_zend_source_release_w05(zend_mir_zend_source *source)
{
	if (source == NULL) {
		return;
	}
	if (source->call_inventory != NULL) {
		zend_mir_frontend_call_inventory *inventory = source->call_inventory;
		source->value_fact_count = inventory->base_value_fact_count;
	}
	zend_mir_frontend_release_call_inventory(source->call_inventory);
	source->call_inventory = NULL;
	source->call_op_array = NULL;
	source->call_ssa = NULL;
	source->script = NULL;
	source->call_site_count = 0;
	source->call_target_count = 0;
	source->call_argument_count = 0;
	source->call_parameter_mode_count = 0;
	source->w05 = false;
}

static uint32_t zend_mir_frontend_call_site_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w05
		? source->call_site_count : 0;
}

static bool zend_mir_frontend_call_site_at(
	const void *context, uint32_t index, zend_mir_source_call_site_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || index >= source->call_site_count) {
		return false;
	}
	inventory = source->call_inventory;
	*out = inventory->sites[index];
	return true;
}

static uint32_t zend_mir_frontend_call_target_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w05
		? source->call_target_count : 0;
}

static bool zend_mir_frontend_call_target_at(
	const void *context, uint32_t index, zend_mir_source_call_target_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || index >= source->call_target_count) {
		return false;
	}
	inventory = source->call_inventory;
	*out = inventory->targets[index].record;
	return true;
}

static uint32_t zend_mir_frontend_call_argument_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w05
		? source->call_argument_count : 0;
}

static bool zend_mir_frontend_call_argument_at(
	const void *context, uint32_t index,
	zend_mir_source_call_argument_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || index >= source->call_argument_count) {
		return false;
	}
	inventory = source->call_inventory;
	*out = inventory->arguments[index];
	return true;
}

static uint32_t zend_mir_frontend_parameter_mode_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	return zend_mir_source_is_initialized(source) && source->w05
		? source->call_parameter_mode_count : 0;
}

static bool zend_mir_frontend_parameter_mode_at(
	const void *context, uint32_t index,
	zend_mir_source_parameter_mode_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || index >= source->call_parameter_mode_count) {
		return false;
	}
	inventory = source->call_inventory;
	*out = inventory->parameter_modes[index];
	return true;
}

static bool zend_mir_frontend_resolve_call_target(
	const void *context, zend_mir_source_call_target_id target_id,
	zend_mir_source_call_target_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || target_id >= source->call_target_count) {
		return false;
	}
	inventory = source->call_inventory;
	if (inventory->targets[target_id].record.kind
			!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
			|| inventory->targets[target_id].function == NULL
			|| (inventory->targets[target_id].record.function_flags_snapshot
				& (ZEND_ACC_CLOSURE | ZEND_ACC_GENERATOR)) != 0) {
		return false;
	}
	*out = inventory->targets[target_id].record;
	return true;
}

static bool zend_mir_frontend_resolve_method_target(
	const void *context, zend_mir_source_call_target_id target_id,
	zend_mir_source_call_target_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	const zend_op_array *op_array;
	uint32_t index;
	bool found_site = false;
	bool open_method;
	if (!zend_mir_source_is_initialized(source) || !source->w10
			|| out == NULL || target_id >= source->call_target_count) {
		return false;
	}
	inventory = source->call_inventory;
	if (inventory->targets[target_id].record.kind
			!= ZEND_MIR_SOURCE_CALL_TARGET_METHOD) {
		return false;
	}
	if (inventory->targets[target_id].function == NULL) {
		op_array = source->call_op_array;
		if (op_array == NULL
				|| inventory->targets[target_id].record.num_args != 0
				|| inventory->targets[target_id].record.required_num_args != 0
				|| inventory->targets[target_id].record.parameter_modes.count != 0) {
			return false;
		}
		open_method = inventory->targets[target_id].record.variadic;
		for (index = 0; index < inventory->site_count; index++) {
			const zend_mir_source_call_site_ref *site =
				&inventory->sites[index];

			if (site->target_id != target_id) {
				continue;
			}
			if (site->init_opline_index >= op_array->last) {
				return false;
			}
			uint8_t init_opcode =
				op_array->opcodes[site->init_opline_index].opcode;
			if (open_method
					? init_opcode != ZEND_INIT_METHOD_CALL
						&& init_opcode != ZEND_INIT_STATIC_METHOD_CALL
						&& init_opcode
							!= ZEND_INIT_PARENT_PROPERTY_HOOK_CALL
					: init_opcode != ZEND_NEW) {
				return false;
			}
			found_site = true;
		}
		if (!found_site) {
			return false;
		}
	} else if (inventory->targets[target_id].function->type
			!= ZEND_USER_FUNCTION
			|| (inventory->targets[target_id].record.function_flags_snapshot
				& (ZEND_ACC_CLOSURE | ZEND_ACC_GENERATOR | ZEND_ACC_ABSTRACT)) != 0) {
		return false;
	}
	*out = inventory->targets[target_id].record;
	return true;
}

static bool zend_mir_frontend_resolve_internal_target(
	const void *context, zend_mir_source_call_target_id target_id,
	zend_mir_source_call_target_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_mir_frontend_call_inventory *inventory;
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL || target_id >= source->call_target_count) {
		return false;
	}
	inventory = source->call_inventory;
	if (inventory->targets[target_id].record.kind
			!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
			|| inventory->targets[target_id].function == NULL
			|| inventory->targets[target_id].function->type
				!= ZEND_INTERNAL_FUNCTION) {
		return false;
	}
	*out = inventory->targets[target_id].record;
	return true;
}

const zend_function *zend_mir_zend_source_internal_function(
	const zend_mir_zend_source *source,
	zend_mir_source_call_target_id target_id)
{
	const zend_mir_frontend_call_inventory *inventory;

	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| target_id >= source->call_target_count) {
		return NULL;
	}
	inventory = source->call_inventory;
	if (inventory->targets[target_id].record.kind
			!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
			|| inventory->targets[target_id].function == NULL
			|| inventory->targets[target_id].function->type
				!= ZEND_INTERNAL_FUNCTION) {
		return NULL;
	}
	return inventory->targets[target_id].function;
}

static uint32_t zend_mir_frontend_call_source_opcode_count(const void *context)
{
	const zend_mir_zend_source *source = context;
	const zend_op_array *op_array;

	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| source->call_op_array == NULL || source->call_ssa == NULL) {
		return 0;
	}
	op_array = source->call_op_array;
	return op_array->last;
}

static bool zend_mir_frontend_call_source_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	const zend_mir_zend_source *source = context;
	const zend_op_array *op_array;
	zend_mir_zend_source original;

	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| source->call_op_array == NULL || source->call_ssa == NULL) {
		return false;
	}
	op_array = source->call_op_array;
	if (index >= op_array->last) {
		return false;
	}
	original = *source;
	original.op_array = source->call_op_array;
	original.ssa = source->call_ssa;
	original.opcode_count = op_array->last;
	return zend_mir_frontend_opcode_at(&original, index, out);
}

bool zend_mir_zend_source_call_view(
	const zend_mir_zend_source *source, zend_mir_source_call_view *out)
{
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->contract_version = ZEND_MIR_W05_CONTRACT_VERSION;
	out->context = source;
	out->call_site_count = zend_mir_frontend_call_site_count;
	out->call_site_at = zend_mir_frontend_call_site_at;
	out->call_target_count = zend_mir_frontend_call_target_count;
	out->call_target_at = zend_mir_frontend_call_target_at;
	out->call_argument_count = zend_mir_frontend_call_argument_count;
	out->call_argument_at = zend_mir_frontend_call_argument_at;
	out->parameter_mode_count = zend_mir_frontend_parameter_mode_count;
	out->parameter_mode_at = zend_mir_frontend_parameter_mode_at;
	out->source_opcode_count = zend_mir_frontend_call_source_opcode_count;
	out->source_opcode_at = zend_mir_frontend_call_source_opcode_at;
	return true;
}

bool zend_mir_zend_source_call_target_resolver(
	const zend_mir_zend_source *source,
	zend_mir_source_call_target_resolver *out)
{
	if (!zend_mir_source_is_initialized(source) || !source->w05
			|| out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->context = source;
	out->resolve_exact_direct_user =
		zend_mir_frontend_resolve_call_target;
	out->resolve_exact_method = zend_mir_frontend_resolve_method_target;
	out->resolve_exact_internal =
		zend_mir_frontend_resolve_internal_target;
	return true;
}
