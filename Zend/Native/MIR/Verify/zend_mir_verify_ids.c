/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_verify_internal.h"

static void zend_mir_verify_bad_identifier(
		zend_mir_verify_context *context, zend_mir_diagnostic_location location,
		zend_mir_verify_code code, const char *message)
{
	context->identifiers_valid = false;
	zend_mir_verify_emit(context, code,
		code == ZEND_MIR_VERIFY_DUPLICATE_ID
			? ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID : ZEND_MIR_DIAGNOSTIC_INVALID_ID,
		location, ZEND_MIR_ID_INVALID, message);
}

static uint32_t zend_mir_verify_value_index(
		const zend_mir_verify_context *context, zend_mir_value_id id)
{
	uint32_t left = 0;
	uint32_t right = context->value_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		zend_mir_value_id current = context->values[middle].record.id;

		if (current < id) {
			left = middle + 1;
		} else if (current > id) {
			right = middle;
		} else {
			return middle;
		}
	}
	return UINT32_MAX;
}

static const zend_mir_verify_constant *zend_mir_verify_find_constant(
		const zend_mir_verify_context *context, zend_mir_value_id id)
{
	uint32_t left = 0;
	uint32_t right = context->constant_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		zend_mir_value_id current = context->constants[middle].record.value_id;

		if (current < id) {
			left = middle + 1;
		} else if (current > id) {
			right = middle;
		} else {
			return &context->constants[middle];
		}
	}
	return NULL;
}

static bool zend_mir_verify_constant_representation(
		const zend_mir_constant_record *constant)
{
	switch (constant->kind) {
		case ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS:
			if (constant->symbol_id != ZEND_MIR_ID_INVALID) {
				return false;
			}
			switch (constant->representation) {
				case ZEND_MIR_REPRESENTATION_I1:
					return (constant->payload_bits & ~UINT64_C(0x1)) == 0;
				case ZEND_MIR_REPRESENTATION_I8:
					return (constant->payload_bits & ~UINT64_C(0xff)) == 0;
				case ZEND_MIR_REPRESENTATION_I16:
					return (constant->payload_bits & ~UINT64_C(0xffff)) == 0;
				case ZEND_MIR_REPRESENTATION_I32:
					return (constant->payload_bits & ~UINT64_C(0xffffffff)) == 0;
				case ZEND_MIR_REPRESENTATION_I64:
				case ZEND_MIR_REPRESENTATION_ZVAL:
					return true;
				default:
					return false;
			}
		case ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS:
			return (constant->representation == ZEND_MIR_REPRESENTATION_DOUBLE
					|| constant->representation == ZEND_MIR_REPRESENTATION_ZVAL)
				&& constant->symbol_id == ZEND_MIR_ID_INVALID;
		case ZEND_MIR_CONSTANT_KIND_NULL_VALUE:
		case ZEND_MIR_CONSTANT_KIND_FALSE_VALUE:
		case ZEND_MIR_CONSTANT_KIND_TRUE_VALUE:
			return constant->representation == ZEND_MIR_REPRESENTATION_ZVAL
				&& constant->payload_bits == 0
				&& constant->symbol_id == ZEND_MIR_ID_INVALID;
		case ZEND_MIR_CONSTANT_KIND_STRING_SYMBOL:
			return constant->representation == ZEND_MIR_REPRESENTATION_ZVAL
				&& constant->payload_bits == 0
				&& zend_mir_id_is_valid(constant->symbol_id);
		case ZEND_MIR_CONSTANT_KIND_SEMANTIC_POINTER_SYMBOL:
			return constant->representation == ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				&& constant->payload_bits == 0
				&& zend_mir_id_is_valid(constant->symbol_id);
		default:
			return false;
	}
}

static void zend_mir_verify_primary_ids(zend_mir_verify_context *context)
{
	uint32_t index;

#define ZEND_MIR_VERIFY_PRIMARY_IDS(field, count, id_expr, location_expr, label) \
	for (index = 0; index < (count); index++) { \
		uint32_t current_id = (id_expr); \
		zend_mir_diagnostic_location current_location = (location_expr); \
		if (!zend_mir_id_is_valid(current_id)) { \
			zend_mir_verify_bad_identifier(context, current_location, \
				ZEND_MIR_VERIFY_INVALID_ID, label " ID is invalid"); \
		} \
		if (index != 0 && current_id == (field)[index - 1].record.id) { \
			zend_mir_verify_bad_identifier(context, current_location, \
				ZEND_MIR_VERIFY_DUPLICATE_ID, label " ID is duplicated"); \
		} \
	}

	ZEND_MIR_VERIFY_PRIMARY_IDS(context->functions, context->function_count,
		context->functions[index].record.id,
		zend_mir_verify_function_location(context, context->functions[index].record.id),
		"function")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->blocks, context->block_count,
		context->blocks[index].record.id,
		zend_mir_verify_block_location(context, context->blocks[index].record.id),
		"block")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->instructions, context->instruction_count,
		context->instructions[index].record.id,
		zend_mir_verify_instruction_location(context, &context->instructions[index].record),
		"instruction")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->values, context->value_count,
		context->values[index].record.id, zend_mir_verify_location(), "value")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->frames, context->frame_count,
		context->frames[index].record.id,
		zend_mir_verify_frame_location(context, &context->frames[index].record),
		"frame state")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->sources, context->source_count,
		context->sources[index].record.id, zend_mir_verify_location(), "source position")
	ZEND_MIR_VERIFY_PRIMARY_IDS(context->source_maps, context->source_map_count,
		context->source_maps[index].record.id, zend_mir_verify_location(), "source map")

#undef ZEND_MIR_VERIFY_PRIMARY_IDS

	for (index = 0; index < context->constant_count; index++) {
		const zend_mir_constant_record *constant = &context->constants[index].record;
		zend_mir_diagnostic_location location = zend_mir_verify_location();

		if (!zend_mir_id_is_valid(constant->value_id)) {
			zend_mir_verify_bad_identifier(context, location,
				ZEND_MIR_VERIFY_INVALID_ID, "constant value ID is invalid");
		}
		if (index != 0
				&& constant->value_id == context->constants[index - 1].record.value_id) {
			zend_mir_verify_bad_identifier(context, location,
				ZEND_MIR_VERIFY_DUPLICATE_ID, "constant value ID is duplicated");
		}
	}
}

void zend_mir_verify_ids(zend_mir_verify_context *context)
{
	uint32_t *definition_counts;
	uint32_t index;

	zend_mir_verify_primary_ids(context);
	if (!context->identifiers_valid || context->halted) {
		return;
	}
	definition_counts = context->value_count == 0 ? NULL
		: zend_mir_verify_allocate(context, context->value_count, sizeof(*definition_counts));
	if (context->value_count != 0 && definition_counts == NULL) {
		return;
	}

	for (index = 0; index < context->function_count; index++) {
		const zend_mir_function_record *function = &context->functions[index].record;
		if (!zend_mir_id_is_valid(function->symbol_id)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FUNCTION,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				zend_mir_verify_function_location(context, function->id),
				ZEND_MIR_ID_INVALID, "function symbol ID is invalid");
		}
	}
	for (index = 0; index < context->block_count; index++) {
		const zend_mir_block_record *block = &context->blocks[index].record;
		if (zend_mir_verify_find_function(context, block->function_id) == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FUNCTION,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_block_location(context, block->id),
				ZEND_MIR_ID_INVALID, "block references an unknown function");
		}
	}
	for (index = 0; index < context->value_count; index++) {
		const zend_mir_value_record *value = &context->values[index].record;
		if ((uint32_t) value->representation >= ZEND_MIR_REPRESENTATION_COUNT
				|| (uint32_t) value->ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_ENUM,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, zend_mir_verify_location(),
				value->id, "value has an unregistered representation or ownership state");
		}
	}
	for (index = 0; index < context->constant_count; index++) {
		const zend_mir_constant_record *constant = &context->constants[index].record;
		const zend_mir_verify_value *value = zend_mir_verify_find_value(
			context, constant->value_id);

		if (value == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_VALUE,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID, zend_mir_verify_location(),
				constant->value_id, "constant references an unknown value");
		} else if ((uint32_t) constant->representation >= ZEND_MIR_REPRESENTATION_COUNT
				|| (uint32_t) constant->kind >= ZEND_MIR_CONSTANT_KIND_COUNT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_ENUM,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, zend_mir_verify_location(),
				constant->value_id, "constant uses an unregistered kind or representation");
		} else if (constant->representation != value->record.representation
				|| !zend_mir_verify_constant_representation(constant)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CONSTANT,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, zend_mir_verify_location(),
				constant->value_id, "constant payload and representation are inconsistent");
		}
	}
	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_instruction_record *instruction = &context->instructions[index].record;
		const zend_mir_verify_block *block = zend_mir_verify_find_block(
			context, instruction->block_id);
		zend_mir_diagnostic_location location =
			zend_mir_verify_instruction_location(context, instruction);

		if (block == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_EDGE,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG, location,
				ZEND_MIR_ID_INVALID, "instruction references an unknown block");
		}
		if ((uint32_t) instruction->opcode >= ZEND_MIR_OPCODE_COUNT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_ENUM,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, location,
				ZEND_MIR_ID_INVALID, "instruction opcode is unregistered");
		}
		if ((uint32_t) instruction->representation >= ZEND_MIR_REPRESENTATION_COUNT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_REPRESENTATION,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, location,
				ZEND_MIR_ID_INVALID, "instruction representation is unregistered");
		}
		if (zend_mir_id_is_valid(instruction->frame_state_id)
				&& zend_mir_verify_find_frame(context, instruction->frame_state_id) == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FRAME,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				ZEND_MIR_ID_INVALID, "instruction references an unknown frame state");
		}
		if (zend_mir_id_is_valid(instruction->source_position_id)
				&& zend_mir_verify_find_source(context, instruction->source_position_id) == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SOURCE,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				ZEND_MIR_ID_INVALID, "instruction references an unknown source position");
		}
		if (zend_mir_id_is_valid(instruction->result_id)) {
			uint32_t value_index = zend_mir_verify_value_index(context, instruction->result_id);
			if (value_index == UINT32_MAX) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_VALUE,
					ZEND_MIR_DIAGNOSTIC_INVALID_ID, location, instruction->result_id,
					"instruction result is not present in the value table");
			} else {
				definition_counts[value_index]++;
				if (definition_counts[value_index] > 1) {
					zend_mir_verify_emit(context, ZEND_MIR_VERIFY_DUPLICATE_DEFINITION,
						ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID, location, instruction->result_id,
						"value has more than one instruction definition");
				}
				if (context->values[value_index].record.representation
						!= instruction->representation) {
					zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_REPRESENTATION,
						ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, location, instruction->result_id,
						"instruction and result value representations differ");
				}
			}
		}
		if (instruction->opcode == ZEND_MIR_OPCODE_CONSTANT
				&& (!zend_mir_id_is_valid(instruction->result_id)
					|| zend_mir_verify_find_constant(context, instruction->result_id) == NULL)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CONSTANT,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, location, instruction->result_id,
				"CONSTANT instruction has no matching constant record");
		} else if (instruction->opcode != ZEND_MIR_OPCODE_CONSTANT
				&& zend_mir_id_is_valid(instruction->result_id)
				&& zend_mir_verify_find_constant(context, instruction->result_id) != NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CONSTANT,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, location, instruction->result_id,
				"constant value is defined by a non-CONSTANT instruction");
		}
	}
}
