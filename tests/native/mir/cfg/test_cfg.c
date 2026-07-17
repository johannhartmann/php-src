#include "Zend/Native/MIR/CFG/zend_mir_cfg.h"
#include "Zend/Native/MIR/CFG/zend_mir_dominance.h"
#include "Zend/Native/MIR/CFG/zend_mir_phi.h"
#include "tests/native/mir/contracts/fixture_host.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ALLOCATION_LIMIT 256
#define TEST_SNAPSHOT_CAPACITY 32768
#define TEST_CHAIN_BLOCK_COUNT 130

typedef struct _test_allocator {
	void *allocations[TEST_ALLOCATION_LIMIT];
	uint32_t allocation_count;
	uint32_t call_count;
	uint32_t fail_at;
} test_allocator;

typedef struct _test_graph {
	zend_mir_function_id function_id;
	zend_mir_block_id entry;
	zend_mir_block_id left;
	zend_mir_block_id merge;
	zend_mir_block_id exit;
	zend_mir_block_id dead;
	zend_mir_instruction_id phi;
	zend_mir_instruction_id merge_branch;
	zend_mir_value_id entry_value;
	zend_mir_value_id left_value;
	zend_mir_value_id phi_value;
} test_graph;

typedef struct _test_asymmetric_view {
	zend_mir_fixture_host host;
	zend_mir_block_id extra_from;
	zend_mir_block_id extra_to;
} test_asymmetric_view;

typedef struct _test_chain_view {
	uint32_t block_count;
} test_chain_view;

static zend_mir_instruction_id test_instruction_in_block(
	const zend_mir_fixture_host *host, zend_mir_block_id block_id);

static void *test_allocate(void *context, size_t size, size_t alignment)
{
	test_allocator *allocator = (test_allocator *) context;
	void *allocation;

	assert(alignment <= _Alignof(max_align_t));
	allocator->call_count++;
	if (allocator->fail_at != 0 && allocator->call_count == allocator->fail_at) {
		return NULL;
	}
	if (allocator->allocation_count >= TEST_ALLOCATION_LIMIT) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation == NULL) {
		return NULL;
	}
	allocator->allocations[allocator->allocation_count++] = allocation;
	return allocation;
}

static void test_reset(void *context)
{
	test_allocator *allocator = (test_allocator *) context;
	uint32_t i;

	for (i = 0; i < allocator->allocation_count; i++) {
		free(allocator->allocations[i]);
		allocator->allocations[i] = NULL;
	}
	allocator->allocation_count = 0;
}

static zend_mir_allocator test_allocator_interface(test_allocator *allocator)
{
	zend_mir_allocator interface;

	interface.context = allocator;
	interface.allocate = test_allocate;
	interface.reset = test_reset;
	return interface;
}

static uint32_t test_asymmetric_predecessor_count(
		const void *context, zend_mir_block_id block_id)
{
	const test_asymmetric_view *asymmetric = (const test_asymmetric_view *) context;
	uint32_t count = asymmetric->host.view.predecessor_count(
		&asymmetric->host, block_id);

	return block_id == asymmetric->extra_to ? count + 1 : count;
}

static bool test_asymmetric_predecessor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	const test_asymmetric_view *asymmetric = (const test_asymmetric_view *) context;
	uint32_t count = asymmetric->host.view.predecessor_count(
		&asymmetric->host, block_id);

	if (block_id == asymmetric->extra_to && index == count && out != NULL) {
		*out = asymmetric->extra_from;
		return true;
	}
	return asymmetric->host.view.predecessor_at(
		&asymmetric->host, block_id, index, out);
}

static uint32_t test_chain_function_count(const void *context)
{
	(void) context;
	return 1;
}

static bool test_chain_function_at(const void *context, uint32_t index,
		zend_mir_function_record *out)
{
	(void) context;
	if (index != 0 || out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->id = 0;
	out->entry_block_id = 0;
	return true;
}

static uint32_t test_chain_block_count(const void *context)
{
	return ((const test_chain_view *) context)->block_count;
}

static bool test_chain_block_at(const void *context, uint32_t index,
		zend_mir_block_record *out)
{
	const test_chain_view *chain = (const test_chain_view *) context;

	if (index >= chain->block_count || out == NULL) {
		return false;
	}
	out->id = index;
	out->function_id = 0;
	return true;
}

static uint32_t test_chain_successor_count(
		const void *context, zend_mir_block_id block_id)
{
	const test_chain_view *chain = (const test_chain_view *) context;

	return block_id < chain->block_count && block_id + 1 < chain->block_count;
}

static bool test_chain_successor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	const test_chain_view *chain = (const test_chain_view *) context;

	if (out == NULL || index != 0 || block_id >= chain->block_count
			|| block_id + 1 >= chain->block_count) {
		return false;
	}
	*out = block_id + 1;
	return true;
}

static uint32_t test_chain_predecessor_count(
		const void *context, zend_mir_block_id block_id)
{
	const test_chain_view *chain = (const test_chain_view *) context;

	return block_id != 0 && block_id < chain->block_count;
}

static bool test_chain_predecessor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	const test_chain_view *chain = (const test_chain_view *) context;

	if (out == NULL || index != 0 || block_id == 0
			|| block_id >= chain->block_count) {
		return false;
	}
	*out = block_id - 1;
	return true;
}

static zend_mir_instruction_id test_add_instruction(zend_mir_fixture_host *host,
		zend_mir_block_id block_id, zend_mir_opcode opcode,
		zend_mir_representation representation, zend_mir_value_id result_id)
{
	zend_mir_instruction_record instruction;
	zend_mir_instruction_id instruction_id;

	memset(&instruction, 0, sizeof(instruction));
	instruction.block_id = block_id;
	instruction.opcode = opcode;
	instruction.representation = representation;
	instruction.result_id = result_id;
	instruction.frame_state_id = ZEND_MIR_ID_INVALID;
	instruction.source_position_id = ZEND_MIR_ID_INVALID;
	assert(host->mutator.add_instruction(
		host->mutator.context, &instruction, &instruction_id));
	return instruction_id;
}

static void test_build_critical_graph(zend_mir_fixture_host *host, test_graph *graph)
{
	zend_mir_instruction_id entry_branch;
	zend_mir_instruction_id left_branch;
	zend_mir_instruction_id dead_branch;

	memset(graph, 0, sizeof(*graph));
	zend_mir_fixture_host_init(host, 41);
	assert(host->mutator.add_function(host->mutator.context, 7, &graph->function_id));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->entry));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->left));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->merge));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->exit));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->dead));
	assert(host->mutator.set_entry_block(
		host->mutator.context, graph->function_id, graph->entry));
	graph->entry_value = zend_mir_value_from_original_ssa(1);
	graph->left_value = zend_mir_value_from_original_ssa(2);
	graph->phi_value = zend_mir_value_from_synthetic(1);
	assert(host->mutator.add_value(host->mutator.context, graph->entry_value,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host->mutator.add_value(host->mutator.context, graph->left_value,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host->mutator.add_value(host->mutator.context, graph->phi_value,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	entry_branch = test_add_instruction(host, graph->entry,
		ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	assert(host->mutator.add_operand(
		host->mutator.context, entry_branch, graph->entry_value));
	left_branch = test_add_instruction(host, graph->left,
		ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	(void) left_branch;
	graph->phi = test_add_instruction(host, graph->merge,
		ZEND_MIR_OPCODE_PHI, ZEND_MIR_REPRESENTATION_ZVAL, graph->phi_value);
	assert(host->mutator.add_operand(
		host->mutator.context, graph->phi, graph->entry_value));
	assert(host->mutator.add_operand(
		host->mutator.context, graph->phi, graph->left_value));
	graph->merge_branch = test_add_instruction(host, graph->merge,
		ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	host->instructions[graph->merge_branch].effects
		= ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_READ_MEMORY);
	host->instructions[graph->merge_branch].reads
		= ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL);
	(void) test_add_instruction(host, graph->exit,
		ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	dead_branch = test_add_instruction(host, graph->dead,
		ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	(void) dead_branch;
	assert(host->mutator.add_edge(host->mutator.context, graph->entry, graph->left));
	assert(host->mutator.add_edge(host->mutator.context, graph->entry, graph->merge));
	assert(host->mutator.add_edge(host->mutator.context, graph->left, graph->merge));
	assert(host->mutator.add_edge(host->mutator.context, graph->merge, graph->exit));
	assert(host->mutator.add_edge(host->mutator.context, graph->dead, graph->dead));
}

static zend_mir_cfg *test_create_cfg(zend_mir_fixture_host *host,
		const test_graph *graph, test_allocator *allocator)
{
	zend_mir_cfg *cfg = NULL;

	assert(zend_mir_cfg_create(&cfg, &host->view, graph->function_id,
		test_allocator_interface(allocator), NULL) == ZEND_MIR_CFG_STATUS_OK);
	assert(cfg != NULL);
	return cfg;
}

static void test_snapshot_append(char *buffer, size_t capacity, size_t *length,
		const char *format, ...)
{
	va_list arguments;
	int written;

	assert(*length < capacity);
	va_start(arguments, format);
	written = vsnprintf(buffer + *length, capacity - *length, format, arguments);
	va_end(arguments);
	assert(written >= 0);
	assert((size_t) written < capacity - *length);
	*length += (size_t) written;
}

static void test_snapshot(const zend_mir_view *view, char *buffer, size_t capacity)
{
	size_t length = 0;
	uint32_t i;

	buffer[0] = '\0';
	test_snapshot_append(buffer, capacity, &length, "b%u/i%u;",
		view->block_count(view->context), view->instruction_count(view->context));
	for (i = 0; i < view->block_count(view->context); i++) {
		zend_mir_block_record block;
		uint32_t slot;
		assert(view->block_at(view->context, i, &block));
		test_snapshot_append(buffer, capacity, &length, "B%u:%u P[",
			block.id, block.function_id);
		for (slot = 0; slot < view->predecessor_count(view->context, block.id); slot++) {
			zend_mir_block_id predecessor;
			assert(view->predecessor_at(view->context, block.id, slot, &predecessor));
			test_snapshot_append(buffer, capacity, &length, "%u,", predecessor);
		}
		test_snapshot_append(buffer, capacity, &length, "]S[");
		for (slot = 0; slot < view->successor_count(view->context, block.id); slot++) {
			zend_mir_block_id successor;
			assert(view->successor_at(view->context, block.id, slot, &successor));
			test_snapshot_append(buffer, capacity, &length, "%u,", successor);
		}
		test_snapshot_append(buffer, capacity, &length, "]; ");
	}
	for (i = 0; i < view->instruction_count(view->context); i++) {
		zend_mir_instruction_record instruction;
		uint32_t operand;
		assert(view->instruction_at(view->context, i, &instruction));
		test_snapshot_append(buffer, capacity, &length,
			"I%u:%u:%u:%u:%u:%llu:%llu:%llu:%llu:%llu[",
			instruction.id, instruction.block_id, (uint32_t) instruction.opcode,
			(uint32_t) instruction.representation, instruction.result_id,
			(unsigned long long) instruction.effects,
			(unsigned long long) instruction.reads,
			(unsigned long long) instruction.writes,
			(unsigned long long) instruction.barriers,
			(unsigned long long) instruction.ownership_actions);
		for (operand = 0; operand < view->instruction_operand_count(
				view->context, instruction.id); operand++) {
			zend_mir_value_id value;
			assert(view->instruction_operand_at(
				view->context, instruction.id, operand, &value));
			test_snapshot_append(buffer, capacity, &length, "%u,", value);
		}
		test_snapshot_append(buffer, capacity, &length, "]; ");
	}
}

static void test_assert_reciprocal(const zend_mir_view *view)
{
	uint32_t block_index;

	for (block_index = 0; block_index < view->block_count(view->context);
			block_index++) {
		zend_mir_block_record block;
		uint32_t slot;
		assert(view->block_at(view->context, block_index, &block));
		for (slot = 0; slot < view->successor_count(view->context, block.id); slot++) {
			zend_mir_block_id successor;
			uint32_t predecessor_slot;
			uint32_t matches = 0;
			assert(view->successor_at(
				view->context, block.id, slot, &successor));
			for (predecessor_slot = 0; predecessor_slot
					< view->predecessor_count(view->context, successor);
					predecessor_slot++) {
				zend_mir_block_id predecessor;
				assert(view->predecessor_at(view->context, successor,
					predecessor_slot, &predecessor));
				matches += predecessor == block.id;
			}
			assert(matches == 1);
		}
		for (slot = 0; slot < view->predecessor_count(view->context, block.id);
				slot++) {
			zend_mir_block_id predecessor;
			uint32_t successor_slot;
			uint32_t matches = 0;
			assert(view->predecessor_at(
				view->context, block.id, slot, &predecessor));
			for (successor_slot = 0; successor_slot
					< view->successor_count(view->context, predecessor);
					successor_slot++) {
				zend_mir_block_id successor;
				assert(view->successor_at(view->context, predecessor,
					successor_slot, &successor));
				matches += successor == block.id;
			}
			assert(matches == 1);
		}
	}
}

static zend_mir_instruction_record test_find_instruction(
		const zend_mir_view *view, zend_mir_instruction_id instruction_id)
{
	zend_mir_instruction_record instruction;
	uint32_t i;

	for (i = 0; i < view->instruction_count(view->context); i++) {
		assert(view->instruction_at(view->context, i, &instruction));
		if (instruction.id == instruction_id) {
			return instruction;
		}
	}
	assert(false);
	memset(&instruction, 0, sizeof(instruction));
	return instruction;
}

static void test_edge_and_phi_operations(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_cfg_phi_incoming incoming;
	zend_mir_block_id predecessor;
	zend_mir_value_id value;
	char before[TEST_SNAPSHOT_CAPACITY];
	char after[TEST_SNAPSHOT_CAPACITY];

	test_build_critical_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &allocator);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(zend_mir_cfg_view(cfg));
	test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
	assert(zend_mir_cfg_add_edge(cfg, graph.entry, graph.merge, NULL, 0)
		== ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE);
	test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
	assert(strcmp(before, after) == 0);

	assert(zend_mir_cfg_remove_edge(cfg, graph.entry, graph.merge)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 0, &predecessor, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(predecessor == graph.left && value == graph.left_value);
	incoming.phi_instruction_id = graph.phi;
	incoming.value_id = graph.entry_value;
	assert(zend_mir_cfg_add_edge(cfg, graph.entry, graph.merge, &incoming, 1)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 0, &predecessor, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(predecessor == graph.left && value == graph.left_value);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 1, &predecessor, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(predecessor == graph.entry && value == graph.entry_value);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(zend_mir_cfg_view(cfg));

	assert(zend_mir_phi_set_incoming(
		cfg, graph.phi, graph.entry, graph.left_value) == ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 1, &predecessor, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(predecessor == graph.entry && value == graph.left_value);
	assert(zend_mir_phi_set_incoming(
		cfg, graph.phi, graph.entry, graph.entry_value) == ZEND_MIR_CFG_STATUS_OK);

	assert(zend_mir_cfg_retarget_edge(
		cfg, graph.left, graph.merge, graph.exit, NULL, 0)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 0, &predecessor, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(predecessor == graph.entry && value == graph.entry_value);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(zend_mir_cfg_view(cfg));
	zend_mir_cfg_destroy(cfg);
}

static void test_edge_split(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	const zend_mir_view *view;
	zend_mir_block_id split;
	zend_mir_block_id observed;
	zend_mir_value_id value;

	test_build_critical_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &allocator);
	assert(zend_mir_cfg_split_edge(cfg, graph.entry, graph.merge, &split)
		== ZEND_MIR_CFG_STATUS_OK);
	view = zend_mir_cfg_view(cfg);
	assert(view->successor_at(view->context, graph.entry, 1, &observed));
	assert(observed == split);
	assert(view->successor_at(view->context, split, 0, &observed));
	assert(observed == graph.merge);
	assert(view->predecessor_at(view->context, graph.merge, 0, &observed));
	assert(observed == split);
	assert(zend_mir_phi_incoming_at(cfg, graph.phi, 0, &observed, &value)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(observed == split && value == graph.entry_value);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(view);
	zend_mir_cfg_destroy(cfg);
}

static void test_block_split(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	const zend_mir_view *view;
	zend_mir_block_id split;
	zend_mir_block_id observed;
	zend_mir_function_record function;
	zend_mir_instruction_record moved;

	test_build_critical_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &allocator);
	assert(zend_mir_cfg_split_block(
		cfg, graph.merge, graph.merge_branch, &split) == ZEND_MIR_CFG_STATUS_OK);
	view = zend_mir_cfg_view(cfg);
	assert(view->function_at(view->context, 0, &function));
	assert(function.entry_block_id == graph.entry);
	assert(view->successor_at(view->context, graph.merge, 0, &observed));
	assert(observed == split);
	assert(view->successor_at(view->context, split, 0, &observed));
	assert(observed == graph.exit);
	moved = test_find_instruction(view, graph.merge_branch);
	assert(moved.block_id == split);
	assert(moved.effects == ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_READ_MEMORY));
	assert(moved.reads == ZEND_MIR_MEMORY_DOMAIN_MASK(
		ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL));
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(view);
	zend_mir_cfg_destroy(cfg);
}

static void test_entry_block_split_preserves_phi_slot(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	const zend_mir_view *view;
	zend_mir_instruction_id entry_instruction;
	zend_mir_block_id split;
	zend_mir_block_id observed;
	zend_mir_value_id value;
	zend_mir_function_record function;

	test_build_critical_graph(&host, &graph);
	entry_instruction = test_instruction_in_block(&host, graph.entry);
	cfg = test_create_cfg(&host, &graph, &allocator);
	assert(zend_mir_cfg_split_block(
		cfg, graph.entry, entry_instruction, &split) == ZEND_MIR_CFG_STATUS_OK);
	view = zend_mir_cfg_view(cfg);
	assert(view->function_at(view->context, 0, &function));
	assert(function.entry_block_id == graph.entry);
	assert(view->successor_at(view->context, graph.entry, 0, &observed));
	assert(observed == split);
	assert(view->successor_at(view->context, split, 0, &observed));
	assert(observed == graph.left);
	assert(view->successor_at(view->context, split, 1, &observed));
	assert(observed == graph.merge);
	assert(zend_mir_phi_incoming_at(
		cfg, graph.phi, 0, &observed, &value) == ZEND_MIR_CFG_STATUS_OK);
	assert(observed == split && value == graph.entry_value);
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	test_assert_reciprocal(view);
	zend_mir_cfg_destroy(cfg);
}

static zend_mir_instruction_id test_instruction_in_block(
		const zend_mir_fixture_host *host, zend_mir_block_id block_id)
{
	uint32_t i;

	for (i = 0; i < host->instruction_count; i++) {
		if (host->instructions[i].block_id == block_id) {
			return host->instructions[i].id;
		}
	}
	assert(false);
	return ZEND_MIR_ID_INVALID;
}

static void test_swap_instruction_ids(zend_mir_fixture_host *host,
		zend_mir_instruction_id first, zend_mir_instruction_id second)
{
	uint32_t first_index = UINT32_MAX;
	uint32_t second_index = UINT32_MAX;
	uint32_t i;

	for (i = 0; i < host->instruction_count; i++) {
		if (host->instructions[i].id == first) {
			first_index = i;
		}
		if (host->instructions[i].id == second) {
			second_index = i;
		}
	}
	assert(first_index != UINT32_MAX && second_index != UINT32_MAX);
	host->instructions[first_index].id = second;
	host->instructions[second_index].id = first;
	for (i = 0; i < host->operand_count; i++) {
		if (host->operands[i].instruction_id == first) {
			host->operands[i].instruction_id = second;
		} else if (host->operands[i].instruction_id == second) {
			host->operands[i].instruction_id = first;
		}
	}
}

static void test_view_order_and_source_are_preserved(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_instruction_id entry_instruction;
	zend_mir_block_id split;
	char source_before[TEST_SNAPSHOT_CAPACITY];
	char source_after[TEST_SNAPSHOT_CAPACITY];
	char derived[TEST_SNAPSHOT_CAPACITY];

	test_build_critical_graph(&host, &graph);
	entry_instruction = test_instruction_in_block(&host, graph.entry);
	test_swap_instruction_ids(&host, entry_instruction, graph.merge_branch);
	graph.merge_branch = entry_instruction;
	test_snapshot(&host.view, source_before, sizeof(source_before));
	cfg = test_create_cfg(&host, &graph, &allocator);
	test_snapshot(zend_mir_cfg_view(cfg), derived, sizeof(derived));
	assert(strcmp(source_before, derived) == 0);
	assert(zend_mir_cfg_split_block(
		cfg, graph.merge, graph.merge_branch, &split) == ZEND_MIR_CFG_STATUS_OK);
	assert(test_find_instruction(zend_mir_cfg_view(cfg), graph.phi).block_id
		== graph.merge);
	assert(test_find_instruction(
		zend_mir_cfg_view(cfg), graph.merge_branch).block_id == split);
	test_snapshot(&host.view, source_after, sizeof(source_after));
	assert(strcmp(source_before, source_after) == 0);
	test_assert_reciprocal(zend_mir_cfg_view(cfg));
	assert(zend_mir_cfg_validate(cfg) == ZEND_MIR_CFG_STATUS_OK);
	zend_mir_cfg_destroy(cfg);
}

static void test_invalid_preconditions_are_atomic(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_block_id split = UINT32_C(0x12345678);
	char before[TEST_SNAPSHOT_CAPACITY];
	char after[TEST_SNAPSHOT_CAPACITY];

	test_build_critical_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &allocator);
	test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
	assert(zend_mir_cfg_add_edge(cfg, graph.dead, graph.merge, NULL, 0)
		== ZEND_MIR_CFG_STATUS_INVALID_PHI);
	assert(zend_mir_cfg_split_block(cfg, graph.merge, graph.phi, &split)
		== ZEND_MIR_CFG_STATUS_INVALID_PHI);
	assert(split == UINT32_C(0x12345678));
	assert(zend_mir_phi_set_incoming(
		cfg, graph.phi, graph.dead, graph.entry_value)
		== ZEND_MIR_CFG_STATUS_INVALID_PHI);
	assert(zend_mir_cfg_split_edge(cfg, graph.left, graph.exit, &split)
		== ZEND_MIR_CFG_STATUS_NOT_FOUND);
	assert(split == UINT32_C(0x12345678));
	test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
	assert(strcmp(before, after) == 0);
	zend_mir_cfg_destroy(cfg);
}

static void test_selected_function_scope_is_enforced(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_function_id foreign_function;
	zend_mir_block_id foreign_block;
	zend_mir_instruction_id foreign_return;
	zend_mir_block_id split = UINT32_C(0x12345678);
	char before[TEST_SNAPSHOT_CAPACITY];
	char after[TEST_SNAPSHOT_CAPACITY];

	test_build_critical_graph(&host, &graph);
	assert(host.mutator.add_function(
		host.mutator.context, 99, &foreign_function));
	assert(host.mutator.add_block(
		host.mutator.context, foreign_function, &foreign_block));
	assert(host.mutator.set_entry_block(
		host.mutator.context, foreign_function, foreign_block));
	foreign_return = test_add_instruction(&host, foreign_block,
		ZEND_MIR_OPCODE_RETURN, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	cfg = test_create_cfg(&host, &graph, &allocator);
	test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
	assert(zend_mir_cfg_split_block(
		cfg, foreign_block, foreign_return, &split)
		== ZEND_MIR_CFG_STATUS_NOT_FOUND);
	assert(zend_mir_cfg_add_edge(
		cfg, foreign_block, foreign_block, NULL, 0)
		== ZEND_MIR_CFG_STATUS_NOT_FOUND);
	assert(split == UINT32_C(0x12345678));
	test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
	assert(strcmp(before, after) == 0);
	zend_mir_cfg_destroy(cfg);
}

static void test_deterministic_transform(void)
{
	zend_mir_fixture_host first_host;
	zend_mir_fixture_host second_host;
	test_graph first_graph;
	test_graph second_graph;
	test_allocator first_allocator = {0};
	test_allocator second_allocator = {0};
	zend_mir_cfg *first_cfg;
	zend_mir_cfg *second_cfg;
	zend_mir_block_id first_split;
	zend_mir_block_id second_split;
	char first[TEST_SNAPSHOT_CAPACITY];
	char second[TEST_SNAPSHOT_CAPACITY];

	test_build_critical_graph(&first_host, &first_graph);
	test_build_critical_graph(&second_host, &second_graph);
	first_cfg = test_create_cfg(&first_host, &first_graph, &first_allocator);
	second_cfg = test_create_cfg(&second_host, &second_graph, &second_allocator);
	assert(zend_mir_cfg_split_edge(first_cfg, first_graph.entry,
		first_graph.merge, &first_split) == ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_cfg_split_edge(second_cfg, second_graph.entry,
		second_graph.merge, &second_split) == ZEND_MIR_CFG_STATUS_OK);
	assert(first_split == second_split);
	test_snapshot(zend_mir_cfg_view(first_cfg), first, sizeof(first));
	test_snapshot(zend_mir_cfg_view(second_cfg), second, sizeof(second));
	assert(strcmp(first, second) == 0);
	zend_mir_cfg_destroy(first_cfg);
	zend_mir_cfg_destroy(second_cfg);
}

static void test_mutation_oom_is_atomic(void)
{
	uint32_t offset;

	for (offset = 1; offset <= 3; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		zend_mir_block_id split;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_cfg_split_edge(cfg, graph.entry, graph.merge, &split)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
	for (offset = 1; offset <= 3; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		zend_mir_block_id split;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_cfg_split_block(cfg, graph.merge, graph.merge_branch, &split)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
	for (offset = 1; offset <= 2; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_cfg_retarget_edge(
			cfg, graph.left, graph.merge, graph.exit, NULL, 0)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
	for (offset = 1; offset <= 2; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_cfg_remove_edge(cfg, graph.entry, graph.merge)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
	for (offset = 1; offset <= 1; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_phi_set_incoming(
			cfg, graph.phi, graph.entry, graph.left_value)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
	for (offset = 1; offset <= 2; offset++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg;
		zend_mir_cfg_phi_incoming incoming;
		char before[TEST_SNAPSHOT_CAPACITY];
		char after[TEST_SNAPSHOT_CAPACITY];

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &allocator);
		assert(zend_mir_cfg_remove_edge(cfg, graph.entry, graph.merge)
			== ZEND_MIR_CFG_STATUS_OK);
		incoming.phi_instruction_id = graph.phi;
		incoming.value_id = graph.entry_value;
		test_snapshot(zend_mir_cfg_view(cfg), before, sizeof(before));
		allocator.fail_at = allocator.call_count + offset;
		assert(zend_mir_cfg_add_edge(
			cfg, graph.entry, graph.merge, &incoming, 1)
			== ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		test_snapshot(zend_mir_cfg_view(cfg), after, sizeof(after));
		assert(strcmp(before, after) == 0);
		zend_mir_cfg_destroy(cfg);
	}
}

static void test_create_oom_and_malformed_input(void)
{
	uint32_t fail_at;
	bool saw_success = false;

	for (fail_at = 1; fail_at < 16; fail_at++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg = NULL;
		zend_mir_cfg_status status;

		test_build_critical_graph(&host, &graph);
		allocator.fail_at = fail_at;
		status = zend_mir_cfg_create(&cfg, &host.view, graph.function_id,
			test_allocator_interface(&allocator), NULL);
		if (status == ZEND_MIR_CFG_STATUS_OK) {
			saw_success = true;
			zend_mir_cfg_destroy(cfg);
			break;
		}
		assert(status == ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		assert(cfg == NULL);
		assert(allocator.allocation_count == 0);
	}
	assert(saw_success);
	{
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator allocator = {0};
		zend_mir_cfg *cfg = NULL;

		test_build_critical_graph(&host, &graph);
		assert(host.edge_count < ZEND_MIR_FIXTURE_MAX_EDGES);
		host.edges[host.edge_count++] = host.edges[0];
		assert(zend_mir_cfg_create(&cfg, &host.view, graph.function_id,
			test_allocator_interface(&allocator), NULL)
			== ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE);
		assert(cfg == NULL);
	}
	{
		test_asymmetric_view asymmetric;
		test_graph graph;
		test_allocator cfg_allocator = {0};
		test_allocator dominance_allocator = {0};
		zend_mir_view malformed;
		zend_mir_cfg *cfg = NULL;
		zend_mir_dominance *dominance = NULL;

		memset(&asymmetric, 0, sizeof(asymmetric));
		test_build_critical_graph(&asymmetric.host, &graph);
		asymmetric.extra_from = graph.dead;
		asymmetric.extra_to = graph.merge;
		malformed = asymmetric.host.view;
		malformed.context = &asymmetric;
		malformed.predecessor_count = test_asymmetric_predecessor_count;
		malformed.predecessor_at = test_asymmetric_predecessor_at;
		assert(zend_mir_cfg_create(&cfg, &malformed, graph.function_id,
			test_allocator_interface(&cfg_allocator), NULL)
			== ZEND_MIR_CFG_STATUS_INVALID_CFG);
		assert(cfg == NULL && cfg_allocator.allocation_count == 0);
		assert(zend_mir_dominance_create(&dominance, &malformed,
			graph.function_id, test_allocator_interface(&dominance_allocator), NULL)
			== ZEND_MIR_CFG_STATUS_INVALID_CFG);
		assert(dominance == NULL && dominance_allocator.allocation_count == 0);
	}
}

static void test_dominance(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator cfg_allocator = {0};
	test_allocator dominance_allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_dominance *dominance = NULL;

	test_build_critical_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &cfg_allocator);
	assert(zend_mir_dominance_create(&dominance, zend_mir_cfg_view(cfg),
		graph.function_id, test_allocator_interface(&dominance_allocator), NULL)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_dominance_is_reachable(dominance, graph.entry));
	assert(zend_mir_dominance_is_reachable(dominance, graph.exit));
	assert(!zend_mir_dominance_is_reachable(dominance, graph.dead));
	assert(zend_mir_dominates(dominance, graph.entry, graph.merge));
	assert(zend_mir_dominates(dominance, graph.merge, graph.exit));
	assert(!zend_mir_dominates(dominance, graph.left, graph.merge));
	assert(zend_mir_dominates(dominance, graph.dead, graph.dead));
	assert(zend_mir_immediate_dominator(dominance, graph.merge) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.exit) == graph.merge);
	assert(zend_mir_immediate_dominator(dominance, graph.dead) == ZEND_MIR_ID_INVALID);
	zend_mir_dominance_destroy(dominance);
	zend_mir_cfg_destroy(cfg);
}

static void test_large_chain_dominance(void)
{
	test_chain_view chain = {TEST_CHAIN_BLOCK_COUNT};
	test_allocator allocator = {0};
	zend_mir_view view;
	zend_mir_dominance *dominance = NULL;
	zend_mir_block_id last = TEST_CHAIN_BLOCK_COUNT - 1;

	memset(&view, 0, sizeof(view));
	view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	view.context = &chain;
	view.function_count = test_chain_function_count;
	view.function_at = test_chain_function_at;
	view.block_count = test_chain_block_count;
	view.block_at = test_chain_block_at;
	view.successor_count = test_chain_successor_count;
	view.successor_at = test_chain_successor_at;
	view.predecessor_count = test_chain_predecessor_count;
	view.predecessor_at = test_chain_predecessor_at;
	assert(zend_mir_dominance_create(&dominance, &view, 0,
		test_allocator_interface(&allocator), NULL) == ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_dominance_is_reachable(dominance, last));
	assert(zend_mir_dominates(dominance, 0, last));
	assert(zend_mir_dominates(dominance, 64, last));
	assert(zend_mir_immediate_dominator(dominance, last) == last - 1);
	zend_mir_dominance_destroy(dominance);
}

static void test_dominance_oom(void)
{
	uint32_t fail_at;
	bool saw_success = false;

	for (fail_at = 1; fail_at < 16; fail_at++) {
		zend_mir_fixture_host host;
		test_graph graph;
		test_allocator cfg_allocator = {0};
		test_allocator dominance_allocator = {0};
		zend_mir_cfg *cfg;
		zend_mir_dominance *dominance = NULL;
		zend_mir_cfg_status status;

		test_build_critical_graph(&host, &graph);
		cfg = test_create_cfg(&host, &graph, &cfg_allocator);
		dominance_allocator.fail_at = fail_at;
		status = zend_mir_dominance_create(&dominance, zend_mir_cfg_view(cfg),
			graph.function_id, test_allocator_interface(&dominance_allocator), NULL);
		if (status == ZEND_MIR_CFG_STATUS_OK) {
			saw_success = true;
			zend_mir_dominance_destroy(dominance);
			zend_mir_cfg_destroy(cfg);
			break;
		}
		assert(status == ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED);
		assert(dominance == NULL);
		assert(dominance_allocator.allocation_count == 0);
		zend_mir_cfg_destroy(cfg);
	}
	assert(saw_success);
}

static void test_diamond_dominance(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator cfg_allocator = {0};
	test_allocator dominance_allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_dominance *dominance = NULL;
	zend_mir_block_id right;

	memset(&graph, 0, sizeof(graph));
	zend_mir_fixture_host_init(&host, 44);
	assert(host.mutator.add_function(host.mutator.context, 10, &graph.function_id));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.entry));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.left));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &right));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.merge));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.exit));
	assert(host.mutator.set_entry_block(
		host.mutator.context, graph.function_id, graph.entry));
	(void) test_add_instruction(&host, graph.entry, ZEND_MIR_OPCODE_COND_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.left, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, right, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.merge, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.exit, ZEND_MIR_OPCODE_RETURN,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_edge(host.mutator.context, graph.entry, graph.left));
	assert(host.mutator.add_edge(host.mutator.context, graph.entry, right));
	assert(host.mutator.add_edge(host.mutator.context, graph.left, graph.merge));
	assert(host.mutator.add_edge(host.mutator.context, right, graph.merge));
	assert(host.mutator.add_edge(host.mutator.context, graph.merge, graph.exit));
	cfg = test_create_cfg(&host, &graph, &cfg_allocator);
	assert(zend_mir_dominance_create(&dominance, zend_mir_cfg_view(cfg),
		graph.function_id, test_allocator_interface(&dominance_allocator), NULL)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_immediate_dominator(dominance, graph.left) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, right) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.merge) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.exit) == graph.merge);
	zend_mir_dominance_destroy(dominance);
	zend_mir_cfg_destroy(cfg);
}

static void test_build_loop_graph(zend_mir_fixture_host *host, test_graph *graph)
{
	zend_mir_instruction_id header_phi;
	zend_mir_instruction_id header_branch;
	zend_mir_instruction_id body_branch;
	zend_mir_value_id initial = zend_mir_value_from_original_ssa(11);
	zend_mir_value_id next = zend_mir_value_from_original_ssa(12);
	zend_mir_value_id result = zend_mir_value_from_synthetic(11);

	memset(graph, 0, sizeof(*graph));
	zend_mir_fixture_host_init(host, 42);
	assert(host->mutator.add_function(host->mutator.context, 8, &graph->function_id));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->entry));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->merge));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->left));
	assert(host->mutator.add_block(host->mutator.context, graph->function_id, &graph->exit));
	assert(host->mutator.set_entry_block(
		host->mutator.context, graph->function_id, graph->entry));
	assert(host->mutator.add_value(host->mutator.context, initial,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host->mutator.add_value(host->mutator.context, next,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host->mutator.add_value(host->mutator.context, result,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	(void) test_add_instruction(host, graph->entry, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	header_phi = test_add_instruction(host, graph->merge, ZEND_MIR_OPCODE_PHI,
		ZEND_MIR_REPRESENTATION_ZVAL, result);
	assert(host->mutator.add_operand(host->mutator.context, header_phi, initial));
	assert(host->mutator.add_operand(host->mutator.context, header_phi, next));
	header_branch = test_add_instruction(host, graph->merge,
		ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
		ZEND_MIR_ID_INVALID);
	assert(host->mutator.add_operand(host->mutator.context, header_branch, result));
	body_branch = test_add_instruction(host, graph->left, ZEND_MIR_OPCODE_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) body_branch;
	(void) test_add_instruction(host, graph->exit, ZEND_MIR_OPCODE_RETURN,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	assert(host->mutator.add_edge(host->mutator.context, graph->entry, graph->merge));
	assert(host->mutator.add_edge(host->mutator.context, graph->merge, graph->left));
	assert(host->mutator.add_edge(host->mutator.context, graph->merge, graph->exit));
	assert(host->mutator.add_edge(host->mutator.context, graph->left, graph->merge));
}

static void test_loop_dominance(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator cfg_allocator = {0};
	test_allocator dominance_allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_dominance *dominance = NULL;

	test_build_loop_graph(&host, &graph);
	cfg = test_create_cfg(&host, &graph, &cfg_allocator);
	assert(zend_mir_dominance_create(&dominance, zend_mir_cfg_view(cfg),
		graph.function_id, test_allocator_interface(&dominance_allocator), NULL)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_immediate_dominator(dominance, graph.merge) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.left) == graph.merge);
	assert(zend_mir_immediate_dominator(dominance, graph.exit) == graph.merge);
	zend_mir_dominance_destroy(dominance);
	zend_mir_cfg_destroy(cfg);
}

static void test_irreducible_dominance(void)
{
	zend_mir_fixture_host host;
	test_graph graph;
	test_allocator cfg_allocator = {0};
	test_allocator dominance_allocator = {0};
	zend_mir_cfg *cfg;
	zend_mir_dominance *dominance = NULL;

	memset(&graph, 0, sizeof(graph));
	zend_mir_fixture_host_init(&host, 43);
	assert(host.mutator.add_function(host.mutator.context, 9, &graph.function_id));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.entry));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.left));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.merge));
	assert(host.mutator.add_block(host.mutator.context, graph.function_id, &graph.exit));
	assert(host.mutator.set_entry_block(
		host.mutator.context, graph.function_id, graph.entry));
	(void) test_add_instruction(&host, graph.entry, ZEND_MIR_OPCODE_COND_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.left, ZEND_MIR_OPCODE_COND_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.merge, ZEND_MIR_OPCODE_COND_BRANCH,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	(void) test_add_instruction(&host, graph.exit, ZEND_MIR_OPCODE_RETURN,
		ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_edge(host.mutator.context, graph.entry, graph.left));
	assert(host.mutator.add_edge(host.mutator.context, graph.entry, graph.merge));
	assert(host.mutator.add_edge(host.mutator.context, graph.left, graph.merge));
	assert(host.mutator.add_edge(host.mutator.context, graph.left, graph.exit));
	assert(host.mutator.add_edge(host.mutator.context, graph.merge, graph.left));
	assert(host.mutator.add_edge(host.mutator.context, graph.merge, graph.exit));
	cfg = test_create_cfg(&host, &graph, &cfg_allocator);
	assert(zend_mir_dominance_create(&dominance, zend_mir_cfg_view(cfg),
		graph.function_id, test_allocator_interface(&dominance_allocator), NULL)
		== ZEND_MIR_CFG_STATUS_OK);
	assert(zend_mir_immediate_dominator(dominance, graph.left) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.merge) == graph.entry);
	assert(zend_mir_immediate_dominator(dominance, graph.exit) == graph.entry);
	zend_mir_dominance_destroy(dominance);
	zend_mir_cfg_destroy(cfg);
}

int main(void)
{
	test_create_oom_and_malformed_input();
	test_edge_and_phi_operations();
	test_edge_split();
	test_block_split();
	test_entry_block_split_preserves_phi_slot();
	test_view_order_and_source_are_preserved();
	test_invalid_preconditions_are_atomic();
	test_selected_function_scope_is_enforced();
	test_deterministic_transform();
	test_mutation_oom_is_atomic();
	test_dominance();
	test_large_chain_dominance();
	test_dominance_oom();
	test_diamond_dominance();
	test_loop_dominance();
	test_irreducible_dominance();
	return 0;
}
