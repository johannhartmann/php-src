#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../../../Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"

#define TEST_CAPACITY 16

typedef struct _test_source {
	zend_mir_source_opcode_ref opcodes[TEST_CAPACITY];
	zend_mir_source_ssa_ref ssa[TEST_CAPACITY];
	zend_mir_source_ssa_use_ref uses[TEST_CAPACITY];
	zend_mir_source_ssa_def_ref defs[TEST_CAPACITY];
	zend_mir_source_literal_ref literals[TEST_CAPACITY];
	uint32_t opcode_count;
	uint32_t ssa_count;
	uint32_t use_count;
	uint32_t def_count;
	uint32_t literal_count;
	bool fail_opcode_at;
} test_source;

#define DEFINE_SOURCE_ACCESSORS(prefix, type, field, count_field) \
	static uint32_t prefix##_count(const void *context) \
	{ \
		return ((const test_source *) context)->count_field; \
	} \
	static bool prefix##_at(const void *context, uint32_t index, type *out) \
	{ \
		const test_source *source = (const test_source *) context; \
		if (out == NULL || index >= source->count_field) { \
			return false; \
		} \
		*out = source->field[index]; \
		return true; \
	}

DEFINE_SOURCE_ACCESSORS(test_opcode_base, zend_mir_source_opcode_ref, opcodes, opcode_count)
DEFINE_SOURCE_ACCESSORS(test_ssa, zend_mir_source_ssa_ref, ssa, ssa_count)
DEFINE_SOURCE_ACCESSORS(test_use, zend_mir_source_ssa_use_ref, uses, use_count)
DEFINE_SOURCE_ACCESSORS(test_def, zend_mir_source_ssa_def_ref, defs, def_count)
DEFINE_SOURCE_ACCESSORS(test_literal, zend_mir_source_literal_ref, literals, literal_count)

static bool test_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	const test_source *source = (const test_source *) context;

	if (source->fail_opcode_at) {
		return false;
	}
	return test_opcode_base_at(context, index, out);
}

static void test_source_init(
	test_source *source, zend_mir_lowering_source_view *view)
{
	memset(source, 0, sizeof(*source));
	memset(view, 0, sizeof(*view));
	view->contract_version = ZEND_MIR_CONTRACT_VERSION;
	view->context = source;
	view->opcode_count = test_opcode_base_count;
	view->opcode_at = test_opcode_at;
	view->ssa_count = test_ssa_count;
	view->ssa_at = test_ssa_at;
	view->ssa_use_count = test_use_count;
	view->ssa_use_at = test_use_at;
	view->ssa_def_count = test_def_count;
	view->ssa_def_at = test_def_at;
	view->literal_count = test_literal_count;
	view->literal_at = test_literal_at;
}

static void test_source_add_opcode(
	test_source *source, uint32_t opline, uint32_t opcode)
{
	zend_mir_source_opcode_ref *entry = &source->opcodes[source->opcode_count++];

	memset(entry, 0, sizeof(*entry));
	entry->opline_index = opline;
	entry->zend_opcode_number = opcode;
	entry->op1.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	entry->op2.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	entry->result.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	entry->source_position_id = opline + 100;
}

typedef enum _test_fault {
	TEST_FAULT_NONE = 0,
	TEST_FAULT_CREATE,
	TEST_FAULT_ADD_FUNCTION,
	TEST_FAULT_ADD_BLOCK,
	TEST_FAULT_SET_ENTRY,
	TEST_FAULT_ADD_INSTRUCTION,
	TEST_FAULT_SEAL,
	TEST_FAULT_FINALIZE,
	TEST_FAULT_STAGE1,
	TEST_FAULT_STAGE2
} test_fault;

typedef struct _test_host test_host;

struct _zend_mir_module {
	test_host *host;
};

struct _test_host {
	struct _zend_mir_module module;
	zend_mir_mutator mutator;
	zend_mir_view view;
	test_fault fault;
	uint32_t create_count;
	uint32_t destroy_count;
	uint32_t write_count;
	uint32_t finalize_count;
	uint32_t stage1_count;
	uint32_t stage2_count;
	uint32_t provider_log[TEST_CAPACITY];
	uint32_t provider_log_count;
	char order[TEST_CAPACITY];
	uint32_t order_count;
	zend_mir_diagnostic diagnostics[TEST_CAPACITY];
	uint32_t diagnostic_count;
};

static void test_record_order(test_host *host, char event)
{
	assert(host->order_count < TEST_CAPACITY);
	host->order[host->order_count++] = event;
}

static bool test_add_function(
	void *context, zend_mir_symbol_id symbol_id, zend_mir_function_id *out)
{
	test_host *host = (test_host *) context;

	assert(symbol_id == 77);
	host->write_count++;
	test_record_order(host, 'F');
	if (host->fault == TEST_FAULT_ADD_FUNCTION) {
		return false;
	}
	*out = 0;
	return true;
}

static bool test_add_block(
	void *context, zend_mir_function_id function_id, zend_mir_block_id *out)
{
	test_host *host = (test_host *) context;

	assert(function_id == 0);
	host->write_count++;
	test_record_order(host, 'B');
	if (host->fault == TEST_FAULT_ADD_BLOCK) {
		return false;
	}
	*out = 0;
	return true;
}

static bool test_set_entry(
	void *context, zend_mir_function_id function_id, zend_mir_block_id block_id)
{
	test_host *host = (test_host *) context;

	assert(function_id == 0);
	assert(block_id == 0);
	host->write_count++;
	test_record_order(host, 'E');
	return host->fault != TEST_FAULT_SET_ENTRY;
}

static bool test_seal(void *context, zend_mir_function_id function_id)
{
	test_host *host = (test_host *) context;

	assert(function_id == 0);
	host->write_count++;
	test_record_order(host, 'S');
	return host->fault != TEST_FAULT_SEAL;
}

static bool test_add_instruction(
	void *context, const zend_mir_instruction_record *record,
	zend_mir_instruction_id *out)
{
	test_host *host = (test_host *) context;

	assert(record != NULL);
	host->write_count++;
	test_record_order(host, 'I');
	if (host->fault == TEST_FAULT_ADD_INSTRUCTION) {
		return false;
	}
	*out = 0;
	return true;
}

static zend_mir_module *test_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	test_host *host = (test_host *) context;

	(void) diagnostics;
	assert(module_id == 42);
	host->create_count++;
	test_record_order(host, 'C');
	if (host->fault == TEST_FAULT_CREATE) {
		return NULL;
	}
	return &host->module;
}

static void test_destroy(void *context, zend_mir_module *module)
{
	test_host *host = (test_host *) context;

	assert(module == &host->module);
	host->destroy_count++;
	test_record_order(host, 'D');
}

static zend_mir_mutator *test_mutator(void *context, zend_mir_module *module)
{
	test_host *host = (test_host *) context;

	assert(module == &host->module);
	return &host->mutator;
}

static const zend_mir_view *test_view(
	void *context, const zend_mir_module *module)
{
	test_host *host = (test_host *) context;

	assert(module == &host->module);
	return &host->view;
}

static bool test_finalize(void *context, zend_mir_module *module)
{
	test_host *host = (test_host *) context;

	assert(module == &host->module);
	host->finalize_count++;
	test_record_order(host, 'Z');
	return host->fault != TEST_FAULT_FINALIZE;
}

static bool test_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	test_host *host = (test_host *) context;

	(void) diagnostics;
	assert(view == &host->view);
	host->stage1_count++;
	test_record_order(host, '1');
	return host->fault != TEST_FAULT_STAGE1;
}

static bool test_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	test_host *host = (test_host *) context;

	(void) diagnostics;
	assert(view == &host->view);
	host->stage2_count++;
	test_record_order(host, '2');
	return host->fault != TEST_FAULT_STAGE2;
}

static bool test_emit(void *context, const zend_mir_diagnostic *diagnostic)
{
	test_host *host = (test_host *) context;

	assert(host->diagnostic_count < TEST_CAPACITY);
	host->diagnostics[host->diagnostic_count++] = *diagnostic;
	return true;
}

static void test_host_init(
	test_host *host, zend_mir_lowering_module_ops *ops,
	zend_mir_diagnostic_sink *sink)
{
	memset(host, 0, sizeof(*host));
	memset(ops, 0, sizeof(*ops));
	memset(sink, 0, sizeof(*sink));
	host->module.host = host;
	host->mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	host->mutator.context = host;
	host->mutator.add_function = test_add_function;
	host->mutator.add_block = test_add_block;
	host->mutator.set_entry_block = test_set_entry;
	host->mutator.add_instruction = test_add_instruction;
	host->mutator.seal_function = test_seal;
	host->view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	ops->context = host;
	ops->create = test_create;
	ops->destroy = test_destroy;
	ops->mutator = test_mutator;
	ops->view = test_view;
	ops->finalize = test_finalize;
	ops->verify_stage1 = test_verify_stage1;
	ops->verify_stage2 = test_verify_stage2;
	sink->context = host;
	sink->emit = test_emit;
	sink->limit = TEST_CAPACITY;
}

typedef struct _test_provider_context {
	zend_mir_lowering_claim claims[4];
	uint32_t claim_count;
	test_host *host;
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code code;
	bool emit_test_instruction;
} test_provider_context;

static uint32_t test_claim_count(const void *context)
{
	return ((const test_provider_context *) context)->claim_count;
}

static bool test_claim_at(
	const void *context, uint32_t index, zend_mir_lowering_claim *out)
{
	const test_provider_context *provider = (const test_provider_context *) context;

	if (out == NULL || index >= provider->claim_count) {
		return false;
	}
	*out = provider->claims[index];
	return true;
}

static zend_mir_lowering_status test_lower(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	const test_provider_context *provider =
		(const test_provider_context *)
			zend_mir_lowering_context_provider_context(context);

	assert(provider != NULL);
	assert(mutator != NULL);
	assert(zend_mir_lowering_context_function_id(context) == 0);
	assert(zend_mir_lowering_context_block_id(context) == 0);
	assert(provider->host->provider_log_count < TEST_CAPACITY);
	provider->host->provider_log[provider->host->provider_log_count++]
		= source_opcode->zend_opcode_number;
	test_record_order(provider->host, 'P');
	if (provider->emit_test_instruction) {
		zend_mir_instruction_record record;
		zend_mir_instruction_id instruction_id;

		memset(&record, 0, sizeof(record));
		if (mutator->add_instruction == NULL
				|| !mutator->add_instruction(
					mutator->context, &record, &instruction_id)) {
			assert(zend_mir_lowering_context_set_provider_failure(
				context, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_MUTATION_FAILED));
			return ZEND_MIR_LOWERING_FAILED;
		}
	}
	if (provider->status != ZEND_MIR_LOWERING_SUCCESS && provider->code != ZEND_MIRL_OK) {
		assert(zend_mir_lowering_context_set_provider_failure(
			context, provider->status, provider->code));
	}
	return provider->status;
}

static zend_mir_lowering_provider test_provider_make(
	uint32_t provider_id, uint32_t family_id, test_provider_context *context)
{
	zend_mir_lowering_provider provider;

	provider.provider_id = provider_id;
	provider.semantic_family_id = family_id;
	provider.context = context;
	provider.claim_count = test_claim_count;
	provider.claim_at = test_claim_at;
	provider.lower = test_lower;
	return provider;
}

static void test_provider_context_init(
	test_provider_context *context, test_host *host,
	uint32_t opcode, uint32_t family_id)
{
	memset(context, 0, sizeof(*context));
	context->claims[0].zend_opcode_number = opcode;
	context->claims[0].semantic_family_id = family_id;
	context->claim_count = 1;
	context->host = host;
	context->status = ZEND_MIR_LOWERING_SUCCESS;
	context->code = ZEND_MIRL_OK;
}

static const zend_mir_lowering_profile_entry test_profile_entries[] = {
	{0, ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{1, ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{2, ZEND_MIR_LOWERING_PROFILE_ACCEPTED},
	{42, ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04},
	{43, ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05},
	{44, ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06},
	{45, ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER},
	{46, ZEND_MIR_LOWERING_PROFILE_REJECTED}
};

static const zend_mir_lowering_profile test_profile = {
	test_profile_entries,
	(uint32_t) (sizeof(test_profile_entries) / sizeof(test_profile_entries[0]))
};

static void test_registry_init_complete(
	zend_mir_lowering_registry *registry,
	test_provider_context contexts[3],
	zend_mir_lowering_provider providers[3],
	test_host *host)
{
	zend_mir_lowering_diagnostic_code code;

	assert(zend_mir_lowering_registry_init(registry, &test_profile, &code));
	test_provider_context_init(&contexts[0], host, 0, 30);
	test_provider_context_init(&contexts[1], host, 1, 10);
	test_provider_context_init(&contexts[2], host, 2, 20);
	providers[0] = test_provider_make(3, 30, &contexts[0]);
	providers[1] = test_provider_make(1, 10, &contexts[1]);
	providers[2] = test_provider_make(2, 20, &contexts[2]);
	assert(zend_mir_lowering_registry_register(registry, &providers[2], &code));
	assert(zend_mir_lowering_registry_register(registry, &providers[0], &code));
	assert(zend_mir_lowering_registry_register(registry, &providers[1], &code));
	assert(zend_mir_lowering_registry_validate(registry, &code));
	assert(code == ZEND_MIRL_OK);
}

static zend_mir_lowering_source_shape test_single_block_shape(void)
{
	zend_mir_lowering_source_shape shape;

	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 1;
	shape.ssa_complete = true;
	return shape;
}

static void test_context_init_checked(
	zend_mir_lowering_context *context,
	const zend_mir_lowering_source_view *source,
	const zend_mir_lowering_source_shape *shape,
	const zend_mir_lowering_registry *registry,
	const zend_mir_lowering_module_ops *ops,
	zend_mir_diagnostic_sink *sink)
{
	assert(zend_mir_lowering_context_init(
		context, source, shape, registry, ops, sink, 42, 77, NULL));
}

static void test_registry_contracts(void)
{
	zend_mir_lowering_registry registry;
	zend_mir_lowering_registry missing;
	test_provider_context contexts[3];
	test_provider_context duplicate_context;
	test_provider_context outside_context;
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_provider provider;
	zend_mir_lowering_provider read_back;
	zend_mir_lowering_provider_array invalid_arrays[2];
	zend_mir_lowering_registry untouched;
	zend_mir_lowering_diagnostic_code code;
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;

	test_host_init(&host, &ops, &sink);
	assert(zend_mir_lowering_registry_init(&registry, &test_profile, &code));
	test_provider_context_init(&contexts[0], &host, 0, 30);
	test_provider_context_init(&contexts[1], &host, 1, 10);
	test_provider_context_init(&contexts[2], &host, 2, 20);
	providers[0] = test_provider_make(3, 30, &contexts[0]);
	providers[1] = test_provider_make(1, 10, &contexts[1]);
	providers[2] = test_provider_make(2, 20, &contexts[2]);
	assert(zend_mir_lowering_registry_register(&registry, &providers[0], &code));
	assert(zend_mir_lowering_registry_register(&registry, &providers[2], &code));

	test_provider_context_init(&duplicate_context, &host, 1, 40);
	provider = test_provider_make(2, 40, &duplicate_context);
	assert(!zend_mir_lowering_registry_register(&registry, &provider, &code));
	assert(code == ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);
	provider = test_provider_make(4, 40, &duplicate_context);
	duplicate_context.claims[0].zend_opcode_number = 0;
	assert(!zend_mir_lowering_registry_register(&registry, &provider, &code));
	assert(code == ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);

	test_provider_context_init(&outside_context, &host, 42, 50);
	provider = test_provider_make(5, 50, &outside_context);
	assert(!zend_mir_lowering_registry_register(&registry, &provider, &code));
	assert(code == ZEND_MIRL_UNKNOWN_PROVIDER);

	assert(zend_mir_lowering_registry_register(&registry, &providers[1], &code));
	assert(zend_mir_lowering_registry_provider_count(&registry) == 3);
	assert(zend_mir_lowering_registry_provider_at(&registry, 0, &read_back));
	assert(read_back.provider_id == 1);
	assert(zend_mir_lowering_registry_provider_at(&registry, 1, &read_back));
	assert(read_back.provider_id == 2);
	assert(zend_mir_lowering_registry_provider_at(&registry, 2, &read_back));
	assert(read_back.provider_id == 3);
	assert(zend_mir_lowering_registry_validate(&registry, &code));
	assert(zend_mir_lowering_registry_validate(&registry, &code));
	assert(zend_mir_lowering_registry_find(&registry, 0)->provider_id == 3);
	assert(zend_mir_lowering_registry_find(&registry, 1)->provider_id == 1);
	assert(zend_mir_lowering_registry_find(&registry, 2)->provider_id == 2);

	memset(&untouched, 0, sizeof(untouched));
	untouched.provider_count = 123;
	invalid_arrays[0].providers = &providers[0];
	invalid_arrays[0].provider_count = 1;
	invalid_arrays[1].providers = &providers[0];
	invalid_arrays[1].provider_count = 1;
	assert(!zend_mir_lowering_registry_construct(
		&untouched, &test_profile, invalid_arrays, 2, &code));
	assert(code == ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);
	assert(untouched.provider_count == 123);

	assert(zend_mir_lowering_registry_init(&missing, &test_profile, &code));
	assert(zend_mir_lowering_registry_register(&missing, &providers[0], &code));
	assert(!zend_mir_lowering_registry_validate(&missing, &code));
	assert(code == ZEND_MIRL_UNKNOWN_PROVIDER);
}

static void test_nop_provider(void)
{
	static const zend_mir_lowering_profile_entry entries[] = {
		{0, ZEND_MIR_LOWERING_PROFILE_ACCEPTED}
	};
	static const zend_mir_lowering_profile profile = {entries, 1};
	zend_mir_lowering_registry registry;
	zend_mir_lowering_diagnostic_code code;

	assert(zend_mir_lowering_registry_init(&registry, &profile, &code));
	assert(zend_mir_lowering_register_nop(&registry, 99, &code));
	assert(zend_mir_lowering_registry_validate(&registry, &code));
	assert(zend_mir_lowering_registry_find(&registry, 0) != NULL);
}

static void test_frozen_w03_profile_and_provider_array_merge(void)
{
	static const uint32_t accepted_opcodes[] = {
		0, 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15,
		16, 17, 18, 19, 20, 21, 31, 51, 52, 62, 70, 170
	};
	const zend_mir_lowering_profile *profile = zend_mir_lowering_w03_profile();
	test_provider_context contexts[25];
	zend_mir_lowering_provider providers[25];
	zend_mir_lowering_provider_array arrays[2];
	zend_mir_lowering_registry registry;
	zend_mir_lowering_diagnostic_code code;
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	uint32_t index;

	test_host_init(&host, &ops, &sink);
	assert(profile != NULL);
	assert(profile->entry_count == 212);
	assert(zend_mir_lowering_profile_find(profile, 45) == NULL);
	assert(zend_mir_lowering_profile_find(profile, 79) == NULL);
	assert(zend_mir_lowering_profile_find(profile, 42)->disposition
		== ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04);
	assert(zend_mir_lowering_profile_find(profile, 59)->disposition
		== ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05);
	assert(zend_mir_lowering_profile_find(profile, 4)->disposition
		== ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06);
	for (index = 0; index < 25; index++) {
		test_provider_context_init(
			&contexts[index], &host, accepted_opcodes[index], index + 1);
		providers[index] = test_provider_make(
			100 - index, index + 1, &contexts[index]);
	}
	arrays[0].providers = &providers[13];
	arrays[0].provider_count = 12;
	arrays[1].providers = &providers[0];
	arrays[1].provider_count = 13;
	assert(zend_mir_lowering_registry_construct(
		&registry, profile, arrays, 2, &code));
	assert(zend_mir_lowering_registry_provider_count(&registry) == 25);
	for (index = 0; index < 25; index++) {
		assert(zend_mir_lowering_registry_find(
			&registry, accepted_opcodes[index]) != NULL);
	}
}

static void test_success_and_dispatch_order(void)
{
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_registry registry;
	test_provider_context contexts[3];
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context;
	zend_mir_lowering_result result;

	test_host_init(&host, &ops, &sink);
	test_source_init(&source, &source_view);
	test_source_add_opcode(&source, 7, 2);
	test_source_add_opcode(&source, 9, 0);
	test_source_add_opcode(&source, 12, 1);
	test_registry_init_complete(&registry, contexts, providers, &host);
	test_context_init_checked(
		&context, &source_view, &shape, &registry, &ops, &sink);
	result = zend_mir_lower_source(&context, &host.mutator);
	assert(zend_mir_lowering_result_is_failure_atomic(&result));
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(result.module == &host.module);
	assert(host.destroy_count == 0);
	assert(host.provider_log_count == 3);
	assert(host.provider_log[0] == 2);
	assert(host.provider_log[1] == 0);
	assert(host.provider_log[2] == 1);
	assert(strcmp(host.order, "CFBEPPPSZ12") == 0);
	test_destroy(&host, result.module);
}

static void test_preflight_deferrals_and_rejections(void)
{
	uint32_t deferred_opcodes[] = {42, 43, 44, 45};
	zend_mir_lowering_diagnostic_code deferred_codes[] = {
		ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED,
		ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED,
		ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED,
		ZEND_MIRL_DEFERRED_OPCODE
	};
	uint32_t index;

	for (index = 0; index < 4; index++) {
		test_host host;
		zend_mir_lowering_module_ops ops;
		zend_mir_diagnostic_sink sink;
		test_source source;
		zend_mir_lowering_source_view source_view;
		zend_mir_lowering_registry registry;
		test_provider_context contexts[3];
		zend_mir_lowering_provider providers[3];
		zend_mir_lowering_source_shape shape = test_single_block_shape();
		zend_mir_lowering_context context;
		zend_mir_lowering_result result;

		test_host_init(&host, &ops, &sink);
		test_source_init(&source, &source_view);
		test_source_add_opcode(&source, 10 + index, deferred_opcodes[index]);
		test_registry_init_complete(&registry, contexts, providers, &host);
		test_context_init_checked(
			&context, &source_view, &shape, &registry, &ops, &sink);
		result = zend_mir_lower_source(&context, &host.mutator);
		assert(zend_mir_lowering_result_is_failure_atomic(&result));
		assert(result.status == ZEND_MIR_LOWERING_DEFERRED);
		assert(result.diagnostic_code == deferred_codes[index]);
		assert(host.create_count == 0);
		assert(host.write_count == 0);
		assert(host.diagnostic_count == 1);
		assert(strstr(host.diagnostics[0].message, "opline=") != NULL);
		assert(strstr(host.diagnostics[0].message, "opcode=") != NULL);
	}

	{
		test_host host;
		zend_mir_lowering_module_ops ops;
		zend_mir_diagnostic_sink sink;
		test_source source;
		zend_mir_lowering_source_view source_view;
		zend_mir_lowering_registry registry;
		test_provider_context contexts[3];
		zend_mir_lowering_provider providers[3];
		zend_mir_lowering_source_shape shape = test_single_block_shape();
		zend_mir_lowering_context context;
		zend_mir_lowering_result result;

		test_host_init(&host, &ops, &sink);
		test_source_init(&source, &source_view);
		test_source_add_opcode(&source, 0, 0);
		test_registry_init_complete(&registry, contexts, providers, &host);
		shape.reachable_block_count = 2;
		test_context_init_checked(
			&context, &source_view, &shape, &registry, &ops, &sink);
		result = zend_mir_lower_source(&context, &host.mutator);
		assert(result.status == ZEND_MIR_LOWERING_DEFERRED);
		assert(result.diagnostic_code == ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);
		assert(host.create_count == 0);
	}
}

static void test_diagnostic_limit_is_hard(void)
{
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_registry registry;
	test_provider_context contexts[3];
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context;
	zend_mir_lowering_result result;

	test_host_init(&host, &ops, &sink);
	sink.limit = 0;
	test_source_init(&source, &source_view);
	test_source_add_opcode(&source, 5, 42);
	test_registry_init_complete(&registry, contexts, providers, &host);
	test_context_init_checked(
		&context, &source_view, &shape, &registry, &ops, &sink);
	result = zend_mir_lower_source(&context, &host.mutator);
	assert(result.status == ZEND_MIR_LOWERING_DEFERRED);
	assert(result.diagnostic_code == ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);
	assert(sink.emitted == 0);
	assert(host.diagnostic_count == 0);
	assert(host.create_count == 0);
}

static void test_diagnostics_require_source_order(void)
{
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_registry registry;
	test_provider_context contexts[3];
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context;
	zend_mir_source_opcode_ref first;
	zend_mir_source_opcode_ref second;
	zend_mir_source_opcode_ref out_of_order;

	test_host_init(&host, &ops, &sink);
	test_source_init(&source, &source_view);
	test_source_add_opcode(&source, 10, 0);
	test_registry_init_complete(&registry, contexts, providers, &host);
	test_context_init_checked(
		&context, &source_view, &shape, &registry, &ops, &sink);
	first = source.opcodes[0];
	second = first;
	second.opline_index = 20;
	out_of_order = first;
	out_of_order.opline_index = 15;
	assert(zend_mir_lowering_emit_diagnostic(
		&context, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_MISSING_PROOF,
		&first, "first"));
	assert(zend_mir_lowering_emit_diagnostic(
		&context, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_MISSING_PROOF,
		&second, "second"));
	assert(!zend_mir_lowering_emit_diagnostic(
		&context, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_MISSING_PROOF,
		&out_of_order, "out of order"));
	assert(host.diagnostic_count == 2);
	assert(host.diagnostics[0].location.source_position_id
		== first.source_position_id);
	assert(host.diagnostics[1].location.source_position_id
		== second.source_position_id);
}

static void test_malformed_source_is_rejected_before_writes(void)
{
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_registry registry;
	test_provider_context contexts[3];
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context;
	zend_mir_lowering_result result;

	test_host_init(&host, &ops, &sink);
	test_source_init(&source, &source_view);
	test_source_add_opcode(&source, 0, 0);
	source.opcodes[0].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].op1.ssa_variable_id = 7;
	test_registry_init_complete(&registry, contexts, providers, &host);
	test_context_init_checked(
		&context, &source_view, &shape, &registry, &ops, &sink);
	result = zend_mir_lower_source(&context, &host.mutator);
	assert(zend_mir_lowering_result_is_failure_atomic(&result));
	assert(result.status == ZEND_MIR_LOWERING_REJECTED);
	assert(result.diagnostic_code == ZEND_MIRL_INVALID_SOURCE);
	assert(host.create_count == 0);
	assert(host.write_count == 0);
}

static void test_provider_failure_is_atomic(void)
{
	test_host host;
	zend_mir_lowering_module_ops ops;
	zend_mir_diagnostic_sink sink;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_registry registry;
	test_provider_context contexts[3];
	zend_mir_lowering_provider providers[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context;
	zend_mir_lowering_result result;

	test_host_init(&host, &ops, &sink);
	test_source_init(&source, &source_view);
	test_source_add_opcode(&source, 4, 1);
	test_registry_init_complete(&registry, contexts, providers, &host);
	contexts[1].status = ZEND_MIR_LOWERING_REJECTED;
	contexts[1].code = ZEND_MIRL_MISSING_PROOF;
	test_context_init_checked(
		&context, &source_view, &shape, &registry, &ops, &sink);
	result = zend_mir_lower_source(&context, &host.mutator);
	assert(zend_mir_lowering_result_is_failure_atomic(&result));
	assert(result.status == ZEND_MIR_LOWERING_REJECTED);
	assert(result.diagnostic_code == ZEND_MIRL_MISSING_PROOF);
	assert(host.destroy_count == 1);
	assert(host.finalize_count == 0);
}

static zend_mir_lowering_diagnostic_code test_fault_code(test_fault fault)
{
	switch (fault) {
		case TEST_FAULT_FINALIZE:
			return ZEND_MIRL_FINALIZE_FAILED;
		case TEST_FAULT_STAGE1:
			return ZEND_MIRL_STAGE1_VERIFY_FAILED;
		case TEST_FAULT_STAGE2:
			return ZEND_MIRL_STAGE2_VERIFY_FAILED;
		default:
			return ZEND_MIRL_MUTATION_FAILED;
	}
}

static void test_fault_injection(void)
{
	test_fault fault;

	for (fault = TEST_FAULT_CREATE; fault <= TEST_FAULT_STAGE2; fault++) {
		test_host host;
		zend_mir_lowering_module_ops ops;
		zend_mir_diagnostic_sink sink;
		test_source source;
		zend_mir_lowering_source_view source_view;
		zend_mir_lowering_registry registry;
		test_provider_context contexts[3];
		zend_mir_lowering_provider providers[3];
		zend_mir_lowering_source_shape shape = test_single_block_shape();
		zend_mir_lowering_context context;
		zend_mir_lowering_result result;

		test_host_init(&host, &ops, &sink);
		host.fault = fault;
		test_source_init(&source, &source_view);
		test_source_add_opcode(&source, 0, 0);
		test_registry_init_complete(&registry, contexts, providers, &host);
		if (fault == TEST_FAULT_ADD_INSTRUCTION) {
			contexts[0].emit_test_instruction = true;
		}
		test_context_init_checked(
			&context, &source_view, &shape, &registry, &ops, &sink);
		result = zend_mir_lower_source(&context, &host.mutator);
		assert(zend_mir_lowering_result_is_failure_atomic(&result));
		assert(result.status == ZEND_MIR_LOWERING_FAILED);
		assert(result.diagnostic_code == test_fault_code(fault));
		assert(result.module == NULL);
		assert(host.destroy_count == (fault == TEST_FAULT_CREATE ? 0U : 1U));
		assert(host.stage2_count == (fault == TEST_FAULT_STAGE2 ? 1U : 0U));
	}
}

static void test_two_live_contexts_are_isolated(void)
{
	test_host host_a;
	test_host host_b;
	zend_mir_lowering_module_ops ops_a;
	zend_mir_lowering_module_ops ops_b;
	zend_mir_diagnostic_sink sink_a;
	zend_mir_diagnostic_sink sink_b;
	test_source source_a;
	test_source source_b;
	zend_mir_lowering_source_view view_a;
	zend_mir_lowering_source_view view_b;
	zend_mir_lowering_registry registry_a;
	zend_mir_lowering_registry registry_b;
	test_provider_context contexts_a[3];
	test_provider_context contexts_b[3];
	zend_mir_lowering_provider providers_a[3];
	zend_mir_lowering_provider providers_b[3];
	zend_mir_lowering_source_shape shape = test_single_block_shape();
	zend_mir_lowering_context context_a;
	zend_mir_lowering_context context_b;
	zend_mir_lowering_result result_a;
	zend_mir_lowering_result result_b;

	test_host_init(&host_a, &ops_a, &sink_a);
	test_host_init(&host_b, &ops_b, &sink_b);
	test_source_init(&source_a, &view_a);
	test_source_init(&source_b, &view_b);
	test_source_add_opcode(&source_a, 0, 1);
	test_source_add_opcode(&source_b, 0, 2);
	test_registry_init_complete(
		&registry_a, contexts_a, providers_a, &host_a);
	test_registry_init_complete(
		&registry_b, contexts_b, providers_b, &host_b);
	test_context_init_checked(
		&context_a, &view_a, &shape, &registry_a, &ops_a, &sink_a);
	test_context_init_checked(
		&context_b, &view_b, &shape, &registry_b, &ops_b, &sink_b);

	result_b = zend_mir_lower_source(&context_b, &host_b.mutator);
	result_a = zend_mir_lower_source(&context_a, &host_a.mutator);
	assert(result_a.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(result_b.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(host_a.provider_log_count == 1);
	assert(host_b.provider_log_count == 1);
	assert(host_a.provider_log[0] == 1);
	assert(host_b.provider_log[0] == 2);
	assert(result_a.module != result_b.module);
	test_destroy(&host_a, result_a.module);
	test_destroy(&host_b, result_b.module);
}

int main(void)
{
	test_registry_contracts();
	test_nop_provider();
	test_frozen_w03_profile_and_provider_array_merge();
	test_success_and_dispatch_order();
	test_preflight_deferrals_and_rejections();
	test_diagnostic_limit_is_hard();
	test_diagnostics_require_source_order();
	test_malformed_source_is_rejected_before_writes();
	test_provider_failure_is_atomic();
	test_fault_injection();
	test_two_live_contexts_are_isolated();
	puts("core lowering tests: PASS");
	return 0;
}
