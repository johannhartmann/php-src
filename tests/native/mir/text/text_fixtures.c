#include "text_fixtures.h"

#include <string.h>

const char *const zend_mir_text_fixture_names[ZEND_MIR_TEXT_FIXTURE_COUNT] = {
	"linear", "diamond", "loop", "ownership", "exception-statepoint", "nested-frame"
};

static void add_function(zend_mir_fixture_host *host, uint32_t blocks)
{
	uint32_t index;
	host->functions[0].id = 0;
	host->functions[0].symbol_id = 100;
	host->functions[0].entry_block_id = 0;
	host->functions[0].flags = 0;
	host->function_count = 1;
	host->sealed[0] = true;
	for (index = 0; index < blocks; index++) {
		host->blocks[index].id = index;
		host->blocks[index].function_id = 0;
	}
	host->block_count = blocks;
}

static void add_value(zend_mir_fixture_host *host, zend_mir_value_id id,
		zend_mir_representation representation, zend_mir_ownership_state ownership)
{
	zend_mir_value_record *record = &host->values[host->value_count++];
	record->id = id;
	record->representation = representation;
	record->ownership = ownership;
}

static void add_constant(zend_mir_fixture_host *host, zend_mir_value_id id, uint64_t bits)
{
	zend_mir_constant_record *record = &host->constants[host->constant_count++];
	record->value_id = id;
	record->representation = ZEND_MIR_REPRESENTATION_I64;
	record->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	record->payload_bits = bits;
	record->symbol_id = ZEND_MIR_ID_INVALID;
}

static zend_mir_instruction_record *add_instruction(zend_mir_fixture_host *host,
		zend_mir_block_id block, zend_mir_opcode opcode, zend_mir_representation representation,
		zend_mir_value_id result)
{
	zend_mir_instruction_record *record = &host->instructions[host->instruction_count];
	memset(record, 0, sizeof(*record));
	record->id = host->instruction_count++;
	record->block_id = block;
	record->opcode = opcode;
	record->representation = representation;
	record->result_id = result;
	record->frame_state_id = ZEND_MIR_ID_INVALID;
	record->source_position_id = ZEND_MIR_ID_INVALID;
	return record;
}

static void add_operand(zend_mir_fixture_host *host, zend_mir_instruction_id instruction,
		zend_mir_value_id value)
{
	host->operands[host->operand_count].instruction_id = instruction;
	host->operands[host->operand_count++].value_id = value;
}

static void add_edge(zend_mir_fixture_host *host, zend_mir_block_id from, zend_mir_block_id to)
{
	host->edges[host->edge_count].from = from;
	host->edges[host->edge_count++].to = to;
}

static void add_source(zend_mir_fixture_host *host, uint32_t line)
{
	zend_mir_source_position_ref *source = &host->source_positions[host->source_position_count];
	source->id = host->source_position_count++;
	source->file_symbol_id = 200;
	source->line = line;
	source->column_start = 1;
	source->column_end = 8;
}

static zend_mir_frame_state_ref default_frame(void)
{
	zend_mir_frame_state_ref frame;
	memset(&frame, 0, sizeof(frame));
	frame.id = 0;
	frame.function_id = 0;
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = 0;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.exception_continuation.kind = ZEND_MIR_CONTINUATION_KIND_ZEND_EXCEPTION;
	frame.exception_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.exception_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.kind = ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame.bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame.suspend_state_id = ZEND_MIR_ID_INVALID;
	frame.code_version_id = 1;
	frame.resume.allowed = false;
	frame.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame.resume.resume_id = ZEND_MIR_ID_INVALID;
	frame.resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame.resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
	frame.canonical = true;
	return frame;
}

static void build_linear(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;
	add_function(host, 1);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_constant(host, 0, 42);
	add_source(host, 10);
	instruction = add_instruction(host, 0, ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I64, 0);
	instruction->source_position_id = 0;
	instruction = add_instruction(host, 0, ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	instruction->source_position_id = 0;
	add_operand(host, instruction->id, 0);
}

static void build_diamond(zend_mir_fixture_host *host)
{
	add_function(host, 4);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_OWNERSHIP_STATE_BORROWED);
	add_value(host, 1, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_value(host, 2, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_value(host, 3, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_constant(host, 0, 1);
	host->constants[0].representation = ZEND_MIR_REPRESENTATION_I1;
	add_constant(host, 1, 11);
	add_constant(host, 2, 22);
	add_edge(host, 0, 1); add_edge(host, 0, 2); add_edge(host, 2, 3); add_edge(host, 1, 3);
	add_instruction(host, 0, ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	add_operand(host, 0, 0);
	add_instruction(host, 1, ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	add_instruction(host, 2, ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	add_instruction(host, 3, ZEND_MIR_OPCODE_PHI, ZEND_MIR_REPRESENTATION_I64, 3);
	add_operand(host, 3, 1); add_operand(host, 3, 2);
	add_instruction(host, 3, ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	add_operand(host, 4, 3);
}

static void build_loop(zend_mir_fixture_host *host)
{
	add_function(host, 3);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_value(host, 1, ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_constant(host, 0, 0);
	add_edge(host, 0, 1); add_edge(host, 1, 1); add_edge(host, 1, 2);
	add_instruction(host, 0, ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	add_instruction(host, 1, ZEND_MIR_OPCODE_PHI, ZEND_MIR_REPRESENTATION_I64, 1);
	add_operand(host, 1, 0); add_operand(host, 1, 1);
	add_instruction(host, 1, ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	add_operand(host, 2, 1);
	add_instruction(host, 2, ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	add_operand(host, 3, 1);
}

static void build_ownership(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;
	add_function(host, 1);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED);
	add_value(host, ZEND_MIR_VALUE_SYNTHETIC_BIT, ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	instruction = add_instruction(host, 0, ZEND_MIR_OPCODE_COPY, ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_VALUE_SYNTHETIC_BIT);
	instruction->effects = ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_WRITE_MEMORY);
	instruction->writes = ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL);
	instruction->ownership_actions = ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_COPY_ADDREF);
	add_operand(host, instruction->id, 0);
	add_instruction(host, 0, ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	add_operand(host, 1, ZEND_MIR_VALUE_SYNTHETIC_BIT);
}

static void build_exception_statepoint(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;
	zend_mir_frame_state_ref frame;
	add_function(host, 1);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	add_source(host, 44);
	frame = default_frame();
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_EXCEPTION_EDGE;
	host->frame_states[host->frame_state_count++] = frame;
	instruction = add_instruction(host, 0, ZEND_MIR_OPCODE_STATEPOINT, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	instruction->effects = ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW) | ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_BAILOUT);
	instruction->reads = ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_EXCEPTION);
	instruction->barriers = ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SAFEPOINT) | ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION);
	add_instruction(host, 0, ZEND_MIR_OPCODE_THROW, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	add_operand(host, 1, 0);
}

static void build_nested_frame(zend_mir_fixture_host *host)
{
	zend_mir_frame_state_ref outer;
	zend_mir_frame_state_ref inner;
	zend_mir_frame_slot_ref *slot;
	add_function(host, 1);
	add_value(host, 0, ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_OWNED);
	slot = &host->frame_slots[host->frame_slot_count++];
	slot->slot_id = 7; slot->value_id = 0; slot->index = 0;
	slot->kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	slot->representation = ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	slot->materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	slot->rooted = true; slot->cleanup_required = true;
	host->roots[host->root_count++] = 7;
	host->cleanups[host->cleanup_count].slot_id = 7;
	host->cleanups[host->cleanup_count].action = ZEND_MIR_CLEANUP_ACTION_DESTROY;
	host->cleanups[host->cleanup_count++].state = ZEND_MIR_CLEANUP_STATE_PENDING;
	outer = default_frame();
	outer.slots.count = 1; outer.roots.count = 1; outer.cleanup_obligations.count = 1;
	host->frame_states[host->frame_state_count++] = outer;
	inner = default_frame(); inner.id = 1; inner.parent_id = 0; inner.opline_index = 7;
	inner.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_NATIVE;
	inner.return_continuation.frame_state_id = 0; inner.return_continuation.opline_index = 8;
	inner.resume.allowed = true; inner.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER;
	inner.resume.resume_id = 9; inner.resume.code_version_id = 1; inner.resume.target_opline_index = 8;
	inner.suspend_kind = ZEND_MIR_SUSPEND_KIND_GENERATOR; inner.suspend_state_id = 3;
	inner.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	inner.slots.count = 1; inner.roots.count = 1; inner.cleanup_obligations.count = 1;
	host->frame_states[host->frame_state_count++] = inner;
	add_instruction(host, 0, ZEND_MIR_OPCODE_STATEPOINT, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID)->frame_state_id = 1;
	add_instruction(host, 0, ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
}

bool zend_mir_text_build_fixture(const char *name, zend_mir_fixture_host *host)
{
	zend_mir_fixture_host_init(host, 1);
	if (strcmp(name, "linear") == 0) build_linear(host);
	else if (strcmp(name, "diamond") == 0) build_diamond(host);
	else if (strcmp(name, "loop") == 0) build_loop(host);
	else if (strcmp(name, "ownership") == 0) build_ownership(host);
	else if (strcmp(name, "exception-statepoint") == 0) build_exception_statepoint(host);
	else if (strcmp(name, "nested-frame") == 0) build_nested_frame(host);
	else return false;
	return true;
}
