#include "verify_fixtures.h"

#include <string.h>

static zend_mir_instruction_record *zend_mir_verify_fixture_instruction(
		zend_mir_fixture_host *host, zend_mir_block_id block,
		zend_mir_opcode opcode, zend_mir_representation representation,
		zend_mir_value_id result)
{
	zend_mir_instruction_record *instruction =
		&host->instructions[host->instruction_count];

	memset(instruction, 0, sizeof(*instruction));
	instruction->id = host->instruction_count++;
	instruction->block_id = block;
	instruction->opcode = opcode;
	instruction->representation = representation;
	instruction->result_id = result;
	instruction->frame_state_id = ZEND_MIR_ID_INVALID;
	instruction->source_position_id = ZEND_MIR_ID_INVALID;
	return instruction;
}

static void zend_mir_verify_fixture_value(zend_mir_fixture_host *host,
		zend_mir_value_id id, zend_mir_representation representation)
{
	zend_mir_value_record *value = &host->values[host->value_count++];
	value->id = id;
	value->representation = representation;
	value->ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
}

static void zend_mir_verify_fixture_constant(zend_mir_fixture_host *host,
		zend_mir_value_id id, zend_mir_representation representation,
		uint64_t payload)
{
	zend_mir_constant_record *constant =
		&host->constants[host->constant_count++];
	constant->value_id = id;
	constant->representation = representation;
	constant->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	constant->payload_bits = payload;
	constant->symbol_id = ZEND_MIR_ID_INVALID;
}

static void zend_mir_verify_fixture_operand(zend_mir_fixture_host *host,
		zend_mir_instruction_id instruction, zend_mir_value_id value)
{
	host->operands[host->operand_count].instruction_id = instruction;
	host->operands[host->operand_count++].value_id = value;
}

static void zend_mir_verify_fixture_edge(zend_mir_fixture_host *host,
		zend_mir_block_id from, zend_mir_block_id to)
{
	host->edges[host->edge_count].from = from;
	host->edges[host->edge_count++].to = to;
}

static void zend_mir_verify_fixture_metadata(zend_mir_fixture_host *host)
{
	zend_mir_frame_state_ref *frame = &host->frame_states[0];
	zend_mir_source_position_ref *source = &host->source_positions[0];
	zend_mir_source_map_ref *map = &host->source_maps[0];

	source->id = 0;
	source->file_symbol_id = 200;
	source->line = 1;
	source->column_start = 1;
	source->column_end = 2;
	host->source_position_count = 1;

	memset(frame, 0, sizeof(*frame));
	frame->id = 0;
	frame->function_id = 0;
	frame->parent_id = ZEND_MIR_ID_INVALID;
	frame->function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame->opline_index = 0;
	frame->opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame->return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame->return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame->exception_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame->exception_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->exception_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame->bailout_continuation.kind = ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame->bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame->suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame->suspend_state_id = ZEND_MIR_ID_INVALID;
	frame->code_version_id = 1;
	frame->resume.allowed = false;
	frame->resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame->resume.resume_id = ZEND_MIR_ID_INVALID;
	frame->resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame->resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame->safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
	frame->canonical = true;
	host->frame_state_count = 1;

	map->id = 0;
	map->source_position_id = 0;
	map->op_array_id = 300;
	map->opline_index = 0;
	map->opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	map->owner_frame_id = 0;
	host->source_map_count = 1;
}

static void zend_mir_verify_fixture_module(
		zend_mir_fixture_host *host, uint32_t block_count)
{
	uint32_t index;
	zend_mir_fixture_host_init(host, 7);
	host->functions[0].id = 0;
	host->functions[0].symbol_id = 100;
	host->functions[0].entry_block_id = 0;
	host->function_count = 1;
	host->sealed[0] = true;
	for (index = 0; index < block_count; index++) {
		host->blocks[index].id = index;
		host->blocks[index].function_id = 0;
	}
	host->block_count = block_count;
	zend_mir_verify_fixture_metadata(host);
}

void zend_mir_verify_fixture_linear(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;

	zend_mir_verify_fixture_module(host, 1);
	zend_mir_verify_fixture_value(host, 0, ZEND_MIR_REPRESENTATION_I64);
	zend_mir_verify_fixture_constant(host, 0, ZEND_MIR_REPRESENTATION_I64, 42);
	instruction = zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_STATEPOINT, ZEND_MIR_REPRESENTATION_VOID,
		ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	(void) zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I64, 0);
	instruction = zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID,
		ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	zend_mir_verify_fixture_operand(host, instruction->id, 0);
}

void zend_mir_verify_fixture_diamond(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;

	zend_mir_verify_fixture_module(host, 4);
	zend_mir_verify_fixture_value(host, 0, ZEND_MIR_REPRESENTATION_I1);
	zend_mir_verify_fixture_value(host, 1, ZEND_MIR_REPRESENTATION_I64);
	zend_mir_verify_fixture_value(host, 2, ZEND_MIR_REPRESENTATION_I64);
	zend_mir_verify_fixture_value(host, 3, ZEND_MIR_REPRESENTATION_I64);
	zend_mir_verify_fixture_constant(host, 0, ZEND_MIR_REPRESENTATION_I1, 1);
	zend_mir_verify_fixture_constant(host, 1, ZEND_MIR_REPRESENTATION_I64, 11);
	zend_mir_verify_fixture_constant(host, 2, ZEND_MIR_REPRESENTATION_I64, 22);
	zend_mir_verify_fixture_edge(host, 0, 1);
	zend_mir_verify_fixture_edge(host, 0, 2);
	zend_mir_verify_fixture_edge(host, 1, 3);
	zend_mir_verify_fixture_edge(host, 2, 3);

	instruction = zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_STATEPOINT, ZEND_MIR_REPRESENTATION_VOID,
		ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	(void) zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I1, 0);
	instruction = zend_mir_verify_fixture_instruction(host, 0,
		ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	zend_mir_verify_fixture_operand(host, instruction->id, 0);
	(void) zend_mir_verify_fixture_instruction(host, 1,
		ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I64, 1);
	(void) zend_mir_verify_fixture_instruction(host, 1,
		ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	(void) zend_mir_verify_fixture_instruction(host, 2,
		ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I64, 2);
	(void) zend_mir_verify_fixture_instruction(host, 2,
		ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	instruction = zend_mir_verify_fixture_instruction(host, 3,
		ZEND_MIR_OPCODE_PHI, ZEND_MIR_REPRESENTATION_I64, 3);
	zend_mir_verify_fixture_operand(host, instruction->id, 1);
	zend_mir_verify_fixture_operand(host, instruction->id, 2);
	instruction = zend_mir_verify_fixture_instruction(host, 3,
		ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID,
		ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	zend_mir_verify_fixture_operand(host, instruction->id, 3);
}
