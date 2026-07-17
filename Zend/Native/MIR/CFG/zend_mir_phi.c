#include "zend_mir_phi.h"

#include "zend_mir_cfg_internal.h"

#include <string.h>

static bool zend_mir_phi_value_exists(const zend_mir_cfg *cfg, zend_mir_value_id value_id)
{
	const zend_mir_view *view = zend_mir_cfg_view(cfg);
	uint32_t i;
	zend_mir_value_record value;

	if (view == NULL || !zend_mir_id_is_valid(value_id)) {
		return false;
	}
	for (i = 0; i < view->value_count(view->context); i++) {
		if (!view->value_at(view->context, i, &value)) {
			return false;
		}
		if (value.id == value_id) {
			return true;
		}
	}
	return false;
}

uint32_t zend_mir_phi_count(const zend_mir_cfg *cfg, zend_mir_block_id block_id)
{
	if (cfg == NULL || !zend_mir_cfg_block_is_selected(cfg, block_id)) {
		return 0;
	}
	return zend_mir_cfg_phi_count_internal(cfg, block_id);
}

zend_mir_cfg_status zend_mir_phi_at(const zend_mir_cfg *cfg,
		zend_mir_block_id block_id, uint32_t index, zend_mir_phi_record *out)
{
	uint32_t found = 0;
	uint32_t i;

	if (cfg == NULL || out == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	if (!zend_mir_cfg_block_is_selected(cfg, block_id)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	for (i = 0; i < cfg->instruction_count; i++) {
		const zend_mir_instruction_record *instruction = &cfg->instructions[i];
		if (instruction->block_id == block_id
				&& instruction->opcode == ZEND_MIR_OPCODE_PHI) {
			if (found == index) {
				out->instruction_id = instruction->id;
				out->block_id = block_id;
				out->result_id = instruction->result_id;
				out->representation = instruction->representation;
				out->incoming_count = zend_mir_cfg_predecessor_count_internal(
					cfg, block_id);
				return ZEND_MIR_CFG_STATUS_OK;
			}
			found++;
		}
	}
	return ZEND_MIR_CFG_STATUS_NOT_FOUND;
}

zend_mir_cfg_status zend_mir_phi_incoming_at(const zend_mir_cfg *cfg,
		zend_mir_instruction_id phi_instruction_id, uint32_t predecessor_slot,
		zend_mir_block_id *predecessor_id, zend_mir_value_id *value_id)
{
	int instruction;
	const zend_mir_view *view;

	if (cfg == NULL || predecessor_id == NULL || value_id == NULL) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	instruction = zend_mir_cfg_find_instruction(cfg, phi_instruction_id);
	if (instruction < 0) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	if (cfg->instructions[instruction].opcode != ZEND_MIR_OPCODE_PHI) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	if (!zend_mir_cfg_block_is_selected(
			cfg, cfg->instructions[instruction].block_id)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	view = zend_mir_cfg_view(cfg);
	if (predecessor_slot >= view->predecessor_count(
			view->context, cfg->instructions[instruction].block_id)
			|| !view->predecessor_at(view->context,
				cfg->instructions[instruction].block_id, predecessor_slot,
				predecessor_id)
			|| !zend_mir_cfg_phi_value_at(cfg, phi_instruction_id,
				predecessor_slot, value_id)) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_cfg_replace_operands(zend_mir_cfg *cfg,
		const zend_mir_cfg_operand *operands, uint32_t operand_count)
{
	if (cfg == NULL || (operand_count != 0 && operands == NULL)) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	cfg->operands = (zend_mir_cfg_operand *) operands;
	cfg->operand_count = operand_count;
	return ZEND_MIR_CFG_STATUS_OK;
}

zend_mir_cfg_status zend_mir_phi_set_incoming(zend_mir_cfg *cfg,
		zend_mir_instruction_id phi_instruction_id, zend_mir_block_id predecessor_id,
		zend_mir_value_id value_id)
{
	int instruction;
	uint32_t predecessor_count;
	uint32_t predecessor_slot;
	uint32_t operand_slot = 0;
	uint32_t i;
	bool found_predecessor = false;
	bool replaced = false;
	zend_mir_cfg_operand *operands;
	zend_mir_cfg_status status;

	if (cfg == NULL || !zend_mir_phi_value_exists(cfg, value_id)) {
		return ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT;
	}
	instruction = zend_mir_cfg_find_instruction(cfg, phi_instruction_id);
	if (instruction < 0) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	if (cfg->instructions[instruction].opcode != ZEND_MIR_OPCODE_PHI) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	if (!zend_mir_cfg_block_is_selected(
			cfg, cfg->instructions[instruction].block_id)) {
		return ZEND_MIR_CFG_STATUS_NOT_FOUND;
	}
	predecessor_count = zend_mir_cfg_predecessor_count_internal(
		cfg, cfg->instructions[instruction].block_id);
	for (predecessor_slot = 0; predecessor_slot < predecessor_count;
			predecessor_slot++) {
		zend_mir_block_id observed;
		if (cfg->view.predecessor_at(cfg->view.context,
				cfg->instructions[instruction].block_id, predecessor_slot,
				&observed) && observed == predecessor_id) {
			found_predecessor = true;
			break;
		}
	}
	if (!found_predecessor || cfg->operand_count == 0) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	operands = (zend_mir_cfg_operand *) zend_mir_cfg_allocate(cfg,
		cfg->operand_count, sizeof(zend_mir_cfg_operand),
		_Alignof(zend_mir_cfg_operand), &status);
	if (operands == NULL) {
		return status;
	}
	memcpy(operands, cfg->operands, cfg->operand_count * sizeof(*operands));
	for (i = 0; i < cfg->operand_count; i++) {
		if (operands[i].instruction_id != phi_instruction_id) {
			continue;
		}
		if (operand_slot == predecessor_slot) {
			operands[i].value_id = value_id;
			replaced = true;
			break;
		}
		operand_slot++;
	}
	if (!replaced) {
		return ZEND_MIR_CFG_STATUS_INVALID_PHI;
	}
	return zend_mir_cfg_replace_operands(cfg, operands, cfg->operand_count);
}
