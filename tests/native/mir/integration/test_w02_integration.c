#include "Zend/Native/MIR/CFG/zend_mir_cfg.h"
#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"
#include "Zend/Native/MIR/Text/zend_mir_dump.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify.h"
#include "tests/native/mir/contracts/fixture_host.h"
#include "tests/native/mir/text/text_fixtures.h"
#include "tests/native/mir/verify/fixtures/verify_fixtures.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTEGRATION_ALLOCATION_LIMIT 256U

typedef struct _integration_buffer {
	char *bytes;
	size_t length;
	size_t capacity;
	size_t chunk;
} integration_buffer;

typedef struct _integration_diagnostics {
	zend_mir_diagnostic records[ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT];
	uint32_t count;
} integration_diagnostics;

typedef struct _integration_allocator {
	void *allocations[INTEGRATION_ALLOCATION_LIMIT];
	uint32_t count;
} integration_allocator;

typedef void (*integration_builder)(zend_mir_fixture_host *host);

typedef struct _integration_positive {
	const char *name;
	integration_builder build;
} integration_positive;

typedef struct _integration_negative {
	const char *name;
	const char *code;
	zend_mir_function_id function_id;
	zend_mir_block_id block_id;
	zend_mir_instruction_id instruction_id;
	void (*mutate)(zend_mir_fixture_host *host);
} integration_negative;

static void integration_apply_action(zend_mir_instruction_record *instruction,
	zend_mir_ownership_action action);
static void integration_cfg_split(void);

static bool integration_write(void *context, const char *bytes, size_t length)
{
	integration_buffer *buffer = (integration_buffer *) context;
	size_t offset = 0;

	if (length > SIZE_MAX - buffer->length
			|| buffer->length + length == SIZE_MAX) {
		return false;
	}
	if (buffer->length + length + 1 > buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
		char *grown;

		while (capacity < buffer->length + length + 1) {
			if (capacity > SIZE_MAX / 2) {
				return false;
			}
			capacity *= 2;
		}
		grown = (char *) realloc(buffer->bytes, capacity);
		if (grown == NULL) {
			return false;
		}
		buffer->bytes = grown;
		buffer->capacity = capacity;
	}
	while (offset < length) {
		size_t part = length - offset;
		if (buffer->chunk != 0 && part > buffer->chunk) {
			part = buffer->chunk;
		}
		memcpy(buffer->bytes + buffer->length, bytes + offset, part);
		buffer->length += part;
		offset += part;
	}
	buffer->bytes[buffer->length] = '\0';
	return true;
}

static bool integration_dump(const zend_mir_view *view, size_t chunk,
		integration_buffer *buffer)
{
	zend_mir_text_writer writer;

	memset(buffer, 0, sizeof(*buffer));
	buffer->chunk = chunk;
	writer.context = buffer;
	writer.write = integration_write;
	return zend_mir_dump_text(view, &writer, NULL);
}

static void integration_buffer_release(integration_buffer *buffer)
{
	free(buffer->bytes);
	memset(buffer, 0, sizeof(*buffer));
}

static uint64_t integration_hash(const integration_buffer *buffer)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	size_t index;

	for (index = 0; index < buffer->length; index++) {
		hash ^= (unsigned char) buffer->bytes[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static bool integration_collect(void *context,
		const zend_mir_diagnostic *diagnostic)
{
	integration_diagnostics *collected =
		(integration_diagnostics *) context;

	if (collected->count < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT) {
		collected->records[collected->count] = *diagnostic;
	}
	collected->count++;
	return true;
}

static bool integration_verify(const zend_mir_view *view,
		integration_diagnostics *collected)
{
	zend_mir_diagnostic_sink sink;

	memset(collected, 0, sizeof(*collected));
	sink.context = collected;
	sink.emit = integration_collect;
	sink.limit = ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT;
	sink.emitted = 0;
	return zend_mir_verify_stage1(view, &sink);
}

#define REVERSE_RECORDS(array, count, type) do { \
	uint32_t reverse_index; \
	for (reverse_index = 0; reverse_index < (count) / 2; reverse_index++) { \
		type temporary = (array)[reverse_index]; \
		(array)[reverse_index] = (array)[(count) - reverse_index - 1]; \
		(array)[(count) - reverse_index - 1] = temporary; \
	} \
} while (0)

static void integration_reverse_storage(zend_mir_fixture_host *host)
{
	REVERSE_RECORDS(host->functions, host->function_count,
		zend_mir_function_record);
	REVERSE_RECORDS(host->blocks, host->block_count, zend_mir_block_record);
	REVERSE_RECORDS(host->values, host->value_count, zend_mir_value_record);
	REVERSE_RECORDS(host->constants, host->constant_count,
		zend_mir_constant_record);
	REVERSE_RECORDS(host->instructions, host->instruction_count,
		zend_mir_instruction_record);
	REVERSE_RECORDS(host->frame_states, host->frame_state_count,
		zend_mir_frame_state_ref);
	REVERSE_RECORDS(host->source_positions, host->source_position_count,
		zend_mir_source_position_ref);
	REVERSE_RECORDS(host->source_maps, host->source_map_count,
		zend_mir_source_map_ref);
}

static void build_linear(zend_mir_fixture_host *host)
{
	zend_mir_verify_fixture_linear(host);
}

static void build_diamond(zend_mir_fixture_host *host)
{
	zend_mir_verify_fixture_diamond(host);
}

static zend_mir_frame_state_id integration_frame_variant(
		zend_mir_fixture_host *host, zend_mir_safepoint_class safepoint_class,
		zend_mir_value_id live_value)
{
	zend_mir_frame_state_ref *frame = &host->frame_states[host->frame_state_count];
	zend_mir_source_map_ref *map = &host->source_maps[host->source_map_count];

	*frame = host->frame_states[0];
	frame->id = host->frame_state_count++;
	frame->safepoint_class = safepoint_class;
	if (zend_mir_id_is_valid(live_value)) {
		zend_mir_frame_slot_ref *slot =
			&host->frame_slots[host->frame_slot_count];

		memset(slot, 0, sizeof(*slot));
		slot->slot_id = host->frame_slot_count;
		slot->value_id = live_value;
		slot->kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
		slot->representation =
			ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
		slot->materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		frame->slots.offset = host->frame_slot_count++;
		frame->slots.count = 1;
	}
	*map = host->source_maps[0];
	map->id = host->source_map_count++;
	map->owner_frame_id = frame->id;
	return frame->id;
}

static void integration_apply_summary(zend_mir_instruction_record *instruction,
		const zend_mir_effect_summary *summary)
{
	zend_mir_effect_summary combined = *summary;
	zend_mir_effect_mask expanded = 0;
	uint32_t effect;

	while ((combined.effects & ~expanded) != 0) {
		zend_mir_effect_mask pending = combined.effects & ~expanded;

		for (effect = 0; effect < ZEND_MIR_EFFECT_COUNT; effect++) {
			zend_mir_effect_summary atomic;
			zend_mir_effect_summary next;
			zend_mir_effect_mask bit = ZEND_MIR_EFFECT_MASK(effect);

			if ((pending & bit) == 0) {
				continue;
			}
			assert(zend_mir_effect_summary_from_effect(
				(zend_mir_effect) effect, &atomic));
			assert(zend_mir_effect_summary_compose(
				&next, &combined, &atomic));
			combined = next;
			expanded |= bit;
		}
	}
	instruction->effects = combined.effects;
	instruction->reads = combined.reads;
	instruction->writes = combined.writes;
	instruction->barriers = combined.barriers;
}

static void build_critical_edge(zend_mir_fixture_host *host)
{
	integration_cfg_split();
	zend_mir_verify_fixture_diamond(host);
}

static zend_mir_instruction_record *integration_instruction(
		zend_mir_fixture_host *host, zend_mir_instruction_id id,
		zend_mir_block_id block, zend_mir_opcode opcode,
		zend_mir_representation representation, zend_mir_value_id result)
{
	zend_mir_instruction_record *instruction = &host->instructions[id];

	memset(instruction, 0, sizeof(*instruction));
	instruction->id = id;
	instruction->block_id = block;
	instruction->opcode = opcode;
	instruction->representation = representation;
	instruction->result_id = result;
	instruction->frame_state_id = ZEND_MIR_ID_INVALID;
	instruction->source_position_id = ZEND_MIR_ID_INVALID;
	return instruction;
}

static void integration_operand(zend_mir_fixture_host *host,
		zend_mir_instruction_id instruction, zend_mir_value_id value)
{
	host->operands[host->operand_count].instruction_id = instruction;
	host->operands[host->operand_count++].value_id = value;
}

static void integration_edge(zend_mir_fixture_host *host,
		zend_mir_block_id from, zend_mir_block_id to)
{
	host->edges[host->edge_count].from = from;
	host->edges[host->edge_count++].to = to;
}

static void build_loop(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;

	zend_mir_verify_fixture_diamond(host);
	host->block_count = 3;
	host->blocks[0].id = 0;
	host->blocks[1].id = 1;
	host->blocks[2].id = 2;
	host->value_count = 3;
	host->values[0].id = 0;
	host->values[0].representation = ZEND_MIR_REPRESENTATION_I1;
	host->values[1].id = 1;
	host->values[1].representation = ZEND_MIR_REPRESENTATION_I64;
	host->values[2].id = 2;
	host->values[2].representation = ZEND_MIR_REPRESENTATION_I64;
	host->constant_count = 2;
	host->constants[0].value_id = 0;
	host->constants[0].representation = ZEND_MIR_REPRESENTATION_I1;
	host->constants[0].payload_bits = 1;
	host->constants[1].value_id = 1;
	host->constants[1].representation = ZEND_MIR_REPRESENTATION_I64;
	host->constants[1].payload_bits = 0;
	host->edge_count = 0;
	integration_edge(host, 0, 1);
	integration_edge(host, 1, 1);
	integration_edge(host, 1, 2);
	host->operand_count = 0;
	instruction = integration_instruction(host, 0, 0,
		ZEND_MIR_OPCODE_STATEPOINT, ZEND_MIR_REPRESENTATION_VOID,
		ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	(void) integration_instruction(host, 1, 0, ZEND_MIR_OPCODE_CONSTANT,
		ZEND_MIR_REPRESENTATION_I1, 0);
	(void) integration_instruction(host, 2, 0, ZEND_MIR_OPCODE_CONSTANT,
		ZEND_MIR_REPRESENTATION_I64, 1);
	(void) integration_instruction(host, 3, 0, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	instruction = integration_instruction(host, 4, 1, ZEND_MIR_OPCODE_PHI,
		ZEND_MIR_REPRESENTATION_I64, 2);
	integration_operand(host, instruction->id, 1);
	integration_operand(host, instruction->id, 2);
	instruction = integration_instruction(host, 5, 1,
		ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	integration_operand(host, instruction->id, 0);
	instruction = integration_instruction(host, 6, 2, ZEND_MIR_OPCODE_RETURN,
		ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	integration_operand(host, instruction->id, 2);
	host->instruction_count = 7;
}

static void build_ownership(zend_mir_fixture_host *host)
{
	zend_mir_verify_fixture_linear(host);
	host->values[1] = host->values[0];
	host->values[1].id = 1;
	host->value_count = 2;
	(void) integration_instruction(host, 2, 0, ZEND_MIR_OPCODE_COPY,
		ZEND_MIR_REPRESENTATION_I64, 1);
	integration_apply_action(&host->instructions[2],
		ZEND_MIR_OWNERSHIP_ACTION_MOVE);
	(void) integration_instruction(host, 3, 0, ZEND_MIR_OPCODE_STATEPOINT,
		ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	integration_apply_action(&host->instructions[3],
		ZEND_MIR_OWNERSHIP_ACTION_DESTROY);
	host->instructions[3].frame_state_id = integration_frame_variant(host,
		ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR, 1);
	host->instructions[3].source_position_id = 0;
	integration_operand(host, 3, 1);
	(void) integration_instruction(host, 4, 0, ZEND_MIR_OPCODE_RETURN,
		ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_ID_INVALID);
	host->instructions[4].frame_state_id = 0;
	host->instructions[4].source_position_id = 0;
	host->instruction_count = 5;
}

static void build_canonicalize(zend_mir_fixture_host *host)
{
	uint32_t index;

	zend_mir_verify_fixture_diamond(host);
	host->values[4] = host->values[1];
	host->values[4].id = 4;
	host->value_count = 5;
	for (index = host->instruction_count; index > 4; index--) {
		host->instructions[index] = host->instructions[index - 1];
		host->instructions[index].id = index;
	}
	for (index = 0; index < host->operand_count; index++) {
		if (host->operands[index].instruction_id >= 4) {
			host->operands[index].instruction_id++;
		}
		if (host->operands[index].instruction_id == 8
				&& host->operands[index].value_id == 1) {
			host->operands[index].value_id = 4;
		}
	}
	(void) integration_instruction(host, 4, 1, ZEND_MIR_OPCODE_CANONICALIZE,
		ZEND_MIR_REPRESENTATION_I64, 4);
	integration_apply_action(&host->instructions[4],
		ZEND_MIR_OWNERSHIP_ACTION_CANONICALIZE);
	integration_operand(host, 4, 1);
	host->instruction_count++;
}

static void build_exception(zend_mir_fixture_host *host)
{
	zend_mir_effect_summary summary;

	zend_mir_verify_fixture_linear(host);
	assert(zend_mir_effect_summary_from_effect(
		ZEND_MIR_EFFECT_THROW, &summary));
	host->instructions[2].opcode = ZEND_MIR_OPCODE_THROW;
	integration_apply_summary(&host->instructions[2], &summary);
	host->instructions[2].frame_state_id = integration_frame_variant(host,
		ZEND_MIR_SAFEPOINT_CLASS_OBSERVER, 0);
}

static void build_bailout(zend_mir_fixture_host *host)
{
	zend_mir_effect_summary bailout;
	zend_mir_effect_summary terminate;
	zend_mir_effect_summary combined;

	zend_mir_verify_fixture_linear(host);
	assert(zend_mir_effect_summary_from_effect(
		ZEND_MIR_EFFECT_BAILOUT, &bailout));
	assert(zend_mir_effect_summary_from_effect(
		ZEND_MIR_EFFECT_TERMINATE, &terminate));
	assert(zend_mir_effect_summary_compose(&combined, &bailout, &terminate));
	host->instructions[2].opcode = ZEND_MIR_OPCODE_UNREACHABLE;
	integration_apply_summary(&host->instructions[2], &combined);
	host->instructions[2].frame_state_id = integration_frame_variant(host,
		ZEND_MIR_SAFEPOINT_CLASS_BAILOUT_HELPER, ZEND_MIR_ID_INVALID);
	host->operand_count = 0;
}

static void build_nested(zend_mir_fixture_host *host)
{
	zend_mir_frame_state_ref *child;

	zend_mir_verify_fixture_linear(host);
	child = &host->frame_states[1];
	*child = host->frame_states[0];
	child->id = 1;
	child->parent_id = 0;
	child->opline_index = 1;
	host->frame_state_count = 2;
	host->source_maps[1] = host->source_maps[0];
	host->source_maps[1].id = 1;
	host->source_maps[1].owner_frame_id = 1;
	host->source_maps[1].opline_index = 1;
	host->source_map_count = 2;
	host->instructions[2].frame_state_id = 1;
}

static void build_generator(zend_mir_fixture_host *host)
{
	zend_mir_frame_state_ref *frame;

	zend_mir_verify_fixture_linear(host);
	frame = &host->frame_states[1];
	*frame = host->frame_states[0];
	frame->id = 1;
	frame->opline_phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	frame->suspend_kind = ZEND_MIR_SUSPEND_KIND_GENERATOR;
	frame->suspend_state_id = 9;
	frame->resume.allowed = true;
	frame->resume.entry_kind =
		ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER;
	frame->resume.resume_id = 8;
	frame->resume.code_version_id = frame->code_version_id;
	frame->resume.target_opline_index = 5;
	frame->safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	host->frame_state_count = 2;
}

static void build_fiber(zend_mir_fixture_host *host)
{
	zend_mir_frame_state_ref *frame;

	zend_mir_verify_fixture_linear(host);
	frame = &host->frame_states[1];
	*frame = host->frame_states[0];
	frame->id = 1;
	frame->opline_phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	frame->suspend_kind = ZEND_MIR_SUSPEND_KIND_FIBER;
	frame->suspend_state_id = 11;
	frame->resume.allowed = true;
	frame->resume.entry_kind =
		ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER;
	frame->resume.resume_id = 10;
	frame->resume.code_version_id = frame->code_version_id;
	frame->resume.target_opline_index = 6;
	frame->safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH;
	host->frame_state_count = 2;
}

static void integration_expect_positive(const integration_positive *test)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host before;
	zend_mir_fixture_host reordered;
	integration_diagnostics diagnostics;
	integration_buffer baseline;
	integration_buffer repeat;
	integration_buffer chunked;
	integration_buffer reverse;

	test->build(&host);
	before = host;
	if (!integration_verify(&host.view, &diagnostics)) {
		uint32_t index;
		fprintf(stderr, "positive fixture rejected: %s\n", test->name);
		for (index = 0; index < diagnostics.count
				&& index < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT; index++) {
			fprintf(stderr, "  %s\n", diagnostics.records[index].message);
		}
		assert(false);
	}
	assert(diagnostics.count == 0);
	assert(memcmp(&before, &host, sizeof(host)) == 0);
	assert(integration_dump(&host.view, 0, &baseline));
	assert(integration_dump(&host.view, 0, &repeat));
	assert(integration_dump(&host.view, 1, &chunked));
	assert(baseline.length == repeat.length);
	assert(baseline.length == chunked.length);
	assert(memcmp(baseline.bytes, repeat.bytes, baseline.length) == 0);
	assert(memcmp(baseline.bytes, chunked.bytes, baseline.length) == 0);

	reordered = host;
	reordered.view.context = &reordered;
	reordered.mutator.context = &reordered;
	integration_reverse_storage(&reordered);
	assert(integration_dump(&reordered.view, 3, &reverse));
	assert(baseline.length == reverse.length);
	assert(memcmp(baseline.bytes, reverse.bytes, baseline.length) == 0);

	printf("positive %-28s fnv1a64=%016llx bytes=%zu\n", test->name,
		(unsigned long long) integration_hash(&baseline), baseline.length);
	integration_buffer_release(&baseline);
	integration_buffer_release(&repeat);
	integration_buffer_release(&chunked);
	integration_buffer_release(&reverse);
}

static void mutate_unknown_opcode(zend_mir_fixture_host *host)
{
	host->instructions[1].opcode = (zend_mir_opcode) ZEND_MIR_W11_OPCODE_COUNT;
}

static void mutate_use_before_definition(zend_mir_fixture_host *host)
{
	host->instructions[1].id = 3;
	host->instructions[2].id = 2;
}

static void mutate_phi_slot(zend_mir_fixture_host *host)
{
	host->operands[2].instruction_id = 8;
}

static void integration_apply_action(zend_mir_instruction_record *instruction,
		zend_mir_ownership_action action)
{
	zend_mir_ownership_transition transition;
	zend_mir_effect_summary combined;
	zend_mir_effect_mask effects;
	uint32_t effect;

	assert(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_OWNED,
		action, &transition) == ZEND_MIR_OWNERSHIP_TRANSITION_OK);
	combined = transition.summary;
	effects = combined.effects;
	for (effect = 0; effect < ZEND_MIR_EFFECT_COUNT; effect++) {
		zend_mir_effect_summary atomic;
		zend_mir_effect_summary next;

		if ((effects & ZEND_MIR_EFFECT_MASK(effect)) == 0) {
			continue;
		}
		assert(zend_mir_effect_summary_from_effect(
			(zend_mir_effect) effect, &atomic));
		assert(zend_mir_effect_summary_compose(&next, &combined, &atomic));
		combined = next;
	}
	integration_apply_summary(instruction, &combined);
	instruction->ownership_actions = ZEND_MIR_OWNERSHIP_ACTION_MASK(action);
}

static void mutate_double_destroy(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record saved_return = host->instructions[2];
	zend_mir_instruction_record *first = &host->instructions[1];
	zend_mir_instruction_record *second = &host->instructions[2];

	memset(first, 0, sizeof(*first));
	first->id = 1;
	first->block_id = 0;
	first->opcode = ZEND_MIR_OPCODE_STATEPOINT;
	first->representation = ZEND_MIR_REPRESENTATION_VOID;
	first->result_id = ZEND_MIR_ID_INVALID;
	first->frame_state_id = 0;
	first->source_position_id = 0;
	integration_apply_action(first, ZEND_MIR_OWNERSHIP_ACTION_DESTROY);
	*second = *first;
	second->id = 2;
	saved_return.id = 3;
	host->instructions[3] = saved_return;
	host->instruction_count = 4;
	host->operands[0].instruction_id = 3;
	host->operands[1].instruction_id = 1;
	host->operands[1].value_id = 0;
	host->operands[2].instruction_id = 2;
	host->operands[2].value_id = 0;
	host->operand_count = 3;
}

static void mutate_missing_frame(zend_mir_fixture_host *host)
{
	host->instructions[2].frame_state_id = ZEND_MIR_ID_INVALID;
}

static void integration_expect_negative(const integration_negative *test)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host before;
	integration_diagnostics diagnostics;
	const zend_mir_diagnostic *matched = NULL;
	uint32_t index;

	if (strcmp(test->name, "invalid-phi-slot") == 0) {
		zend_mir_verify_fixture_diamond(&host);
	} else {
		zend_mir_verify_fixture_linear(&host);
	}
	test->mutate(&host);
	before = host;
	assert(!integration_verify(&host.view, &diagnostics));
	assert(memcmp(&before, &host, sizeof(host)) == 0);
	for (index = 0; index < diagnostics.count
			&& index < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT; index++) {
		if (strstr(diagnostics.records[index].message, test->code) != NULL) {
			matched = &diagnostics.records[index];
			break;
		}
	}
	assert(matched != NULL);
	assert(matched->location.function_id == test->function_id);
	assert(matched->location.block_id == test->block_id);
	assert(matched->location.instruction_id == test->instruction_id);
	printf("negative %-28s code=%s function=f%u block=b%u instruction=i%u\n",
		test->name, test->code, test->function_id, test->block_id,
		test->instruction_id);
}

static void *integration_allocate(void *context, size_t size, size_t alignment)
{
	integration_allocator *allocator = (integration_allocator *) context;
	void *allocation;

	if (alignment > _Alignof(max_align_t)
			|| allocator->count >= INTEGRATION_ALLOCATION_LIMIT) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation == NULL) {
		return NULL;
	}
	allocator->allocations[allocator->count++] = allocation;
	return allocation;
}

static void integration_reset(void *context)
{
	integration_allocator *allocator = (integration_allocator *) context;
	uint32_t index;

	for (index = 0; index < allocator->count; index++) {
		free(allocator->allocations[index]);
	}
	allocator->count = 0;
}

static zend_mir_allocator integration_allocator_interface(
		integration_allocator *allocator)
{
	zend_mir_allocator interface;

	interface.context = allocator;
	interface.allocate = integration_allocate;
	interface.reset = integration_reset;
	return interface;
}

static void integration_core_dump(size_t chunk_size, integration_buffer *dump)
{
	integration_allocator storage = {{0}, 0};
	zend_mir_module *module;
	zend_mir_allocator allocator = integration_allocator_interface(&storage);
	zend_mir_mutator *mutator;
	zend_mir_function_id function;
	zend_mir_block_id block;
	zend_mir_value_id value = 0;
	zend_mir_instruction_id instruction;
	zend_mir_constant_record constant;
	zend_mir_instruction_record record;
	const zend_mir_view *view;

	module = zend_mir_module_create(21, &allocator, chunk_size, NULL, NULL);
	assert(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	assert(mutator->add_function(mutator->context, 100, &function));
	assert(mutator->add_block(mutator->context, function, &block));
	assert(mutator->set_entry_block(mutator->context, function, block));
	assert(mutator->add_value(mutator->context, value,
		ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	memset(&constant, 0, sizeof(constant));
	constant.value_id = value;
	constant.representation = ZEND_MIR_REPRESENTATION_I64;
	constant.kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	constant.payload_bits = 42;
	constant.symbol_id = ZEND_MIR_ID_INVALID;
	assert(mutator->add_constant(mutator->context, &constant));
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = block;
	record.opcode = ZEND_MIR_OPCODE_CONSTANT;
	record.representation = ZEND_MIR_REPRESENTATION_I64;
	record.result_id = value;
	record.frame_state_id = ZEND_MIR_ID_INVALID;
	record.source_position_id = ZEND_MIR_ID_INVALID;
	assert(mutator->add_instruction(mutator->context, &record, &instruction));
	assert(mutator->seal_function(mutator->context, function));
	assert(zend_mir_module_finalize(module));
	view = zend_mir_module_get_view(module);
	assert(view != NULL && view->function_count(view->context) == 1);
	assert(view->instruction_count(view->context) == 1);
	assert(integration_dump(view, 0, dump));
	zend_mir_module_destroy(module);
}

static void integration_core_storage(void)
{
	integration_buffer small_chunks;
	integration_buffer large_chunks;

	integration_core_dump(64, &small_chunks);
	integration_core_dump(16384, &large_chunks);
	assert(small_chunks.length == large_chunks.length);
	assert(memcmp(small_chunks.bytes, large_chunks.bytes,
		small_chunks.length) == 0);
	integration_buffer_release(&small_chunks);
	integration_buffer_release(&large_chunks);
}

static void integration_cfg_split(void)
{
	zend_mir_fixture_host host;
	integration_allocator storage = {{0}, 0};
	zend_mir_cfg *cfg = NULL;
	zend_mir_block_id split = ZEND_MIR_ID_INVALID;

	zend_mir_verify_fixture_diamond(&host);
	assert(zend_mir_cfg_create(&cfg, &host.view, 0,
		integration_allocator_interface(&storage), NULL)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_cfg_split_edge(cfg, 0, 1, &split)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(split != ZEND_MIR_ID_INVALID);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	zend_mir_cfg_destroy(cfg);
}

int main(void)
{
	static const integration_positive positive[] = {
		{"linear-values", build_linear},
		{"diamond-with-phi", build_diamond},
		{"loop-with-backedge-phi", build_loop},
		{"critical-edge-split", build_critical_edge},
		{"ownership-move-destroy", build_ownership},
		{"canonicalize-before-phi", build_canonicalize},
		{"exception-statepoint", build_exception},
		{"bailout-terminal", build_bailout},
		{"nested-frame-state", build_nested},
		{"generator-suspend-metadata", build_generator},
		{"fiber-switch-metadata", build_fiber},
	};
	static const integration_negative negative[] = {
		{"invalid-unknown-op", "MIRV0102", 0, 0, 1,
			mutate_unknown_opcode},
		{"invalid-use-before-def", "MIRV0300", 0, 0, 2,
			mutate_use_before_definition},
		{"invalid-phi-slot", "MIRV0208", 0, 3, 7, mutate_phi_slot},
		{"invalid-double-destroy", "MIRV0404", 0, 0, 2,
			mutate_double_destroy},
		{"invalid-missing-frame-state", "MIRV0507", 0, 0, 2,
			mutate_missing_frame},
	};
	uint32_t index;

	integration_core_storage();
	for (index = 0; index < sizeof(positive) / sizeof(positive[0]); index++) {
		integration_expect_positive(&positive[index]);
	}
	for (index = 0; index < sizeof(negative) / sizeof(negative[0]); index++) {
		integration_expect_negative(&negative[index]);
	}
	puts("W02 integration tests: ok");
	return 0;
}
