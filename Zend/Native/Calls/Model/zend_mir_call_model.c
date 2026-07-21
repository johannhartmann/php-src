#include "zend_mir_call_model.h"

#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "../../Lowering/Core/zend_mir_lowering_internal.h"
#include "../../Lowering/Frontend/zend_mir_zend_source.h"
#include "../../MIR/Core/zend_mir_module_internal.h"

typedef struct _zend_mir_w05_plan {
	zend_mir_call_plan public_plan;
	zend_mir_call_plan_entry *entries;
	zend_mir_source_call_site_ref *sites;
	zend_mir_source_call_target_ref *targets;
	zend_mir_source_call_argument_ref *arguments;
	zend_mir_value_id *values;
	zend_mir_value_id *results;
	zend_mir_block_id *blocks;
	uint32_t site_count;
	uint32_t target_count;
	uint32_t argument_count;
} zend_mir_w05_plan;

#define W05_EFFECTS \
	(ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_READ_MEMORY) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_WRITE_MEMORY) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_BAILOUT) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_ALLOCATE) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_INTERNAL) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_PHP) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_REENTER_PHP) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_RUN_DESTRUCTOR) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_OBSERVE_FRAME) \
	| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY))
#define W05_READS \
	(ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_ARGS) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_LOCALS) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_TEMPS) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_CALL_CHAIN) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_RUNTIME_SYMBOL_TABLE) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_RUNTIME_CACHE) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ARRAY) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_OBJECT) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_STRING) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_REFERENCE) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_GC_METADATA) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_EXCEPTION) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_OBSERVER) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT) \
	| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_FUNCTION_TABLE))
#define W05_WRITES W05_READS
#define W05_BARRIERS \
	(ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SAFEPOINT) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_REENTRANCY) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_BAILOUT) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT))

typedef enum _zend_mir_w05_allocation_kind {
	ZEND_MIR_W05_ALLOCATION_PLANNER,
	ZEND_MIR_W05_ALLOCATION_TARGET_SNAPSHOT,
	ZEND_MIR_W05_ALLOCATION_ARGUMENT_TABLE
} zend_mir_w05_allocation_kind;

#ifdef ZEND_MIR_W05_TEST_FAULTS
static zend_mir_w05_test_fault zend_mir_w05_test_fault_state =
	ZEND_MIR_W05_TEST_FAULT_NONE;

void zend_mir_w05_test_set_fault(zend_mir_w05_test_fault fault)
{
	zend_mir_w05_test_fault_state = fault;
}

static bool zend_mir_w05_test_fault_is(zend_mir_w05_test_fault fault)
{
	return zend_mir_w05_test_fault_state == fault;
}
#else
# define zend_mir_w05_test_fault_is(fault) false
#endif

static void *zend_mir_w05_calloc(
	uint32_t count, size_t size, zend_mir_w05_allocation_kind kind)
{
	if (size != 0 && (size_t) count > SIZE_MAX / size) {
		return NULL;
	}
#ifdef ZEND_MIR_W05_TEST_FAULTS
	if ((kind == ZEND_MIR_W05_ALLOCATION_PLANNER
			&& zend_mir_w05_test_fault_is(
				ZEND_MIR_W05_TEST_FAULT_PLANNER_ALLOCATION))
			|| (kind == ZEND_MIR_W05_ALLOCATION_TARGET_SNAPSHOT
				&& zend_mir_w05_test_fault_is(
					ZEND_MIR_W05_TEST_FAULT_TARGET_SNAPSHOT))
			|| (kind == ZEND_MIR_W05_ALLOCATION_ARGUMENT_TABLE
				&& zend_mir_w05_test_fault_is(
					ZEND_MIR_W05_TEST_FAULT_ARGUMENT_TABLE))) {
		return NULL;
	}
#else
	(void) kind;
#endif
	return calloc(count, size);
}

static bool zend_mir_w05_target_equals(
	const zend_mir_source_call_target_ref *left,
	const zend_mir_source_call_target_ref *right)
{
	return left->id == right->id
		&& left->kind == right->kind
		&& left->function_symbol_id == right->function_symbol_id
		&& left->op_array_id == right->op_array_id
		&& left->num_args == right->num_args
		&& left->required_num_args == right->required_num_args
		&& left->function_flags_snapshot == right->function_flags_snapshot
		&& left->parameter_modes.offset == right->parameter_modes.offset
		&& left->parameter_modes.count == right->parameter_modes.count
		&& left->variadic == right->variadic
		&& left->returns_by_reference == right->returns_by_reference;
}

static bool zend_mir_w05_target_parameter_modes_are_by_value(
	const zend_mir_source_call_view *calls,
	const zend_mir_source_call_target_ref *target)
{
	uint32_t count;
	uint32_t index;

	if (calls->parameter_mode_count == NULL
			|| calls->parameter_mode_at == NULL) {
		return false;
	}
	count = calls->parameter_mode_count(calls->context);
	if (target->parameter_modes.offset > count
			|| target->parameter_modes.count
				> count - target->parameter_modes.offset
			|| target->parameter_modes.count != target->num_args) {
		return false;
	}
	for (index = 0; index < target->parameter_modes.count; index++) {
		zend_mir_source_parameter_mode_ref mode;
		if (!calls->parameter_mode_at(
				calls->context, target->parameter_modes.offset + index,
				&mode)
				|| mode.target_id != target->id
				|| mode.ordinal != index
				|| mode.mode != ZEND_MIR_SOURCE_PARAMETER_BY_VALUE) {
			return false;
		}
	}
	return true;
}

static zend_mir_w05_lowering_result zend_mir_w05_failure(
	zend_mir_lowering_status status, zend_mir_lowering_diagnostic_code code)
{
	zend_mir_w05_lowering_result result;

	memset(&result, 0, sizeof(result));
	result.lowering.status = status;
	result.lowering.diagnostic_code = code;
	return result;
}

static void zend_mir_w05_plan_release(zend_mir_w05_plan *plan)
{
	if (plan == NULL) {
		return;
	}
	free(plan->entries);
	free(plan->sites);
	free(plan->targets);
	free(plan->arguments);
	free(plan->values);
	free(plan->results);
	free(plan->blocks);
	memset(plan, 0, sizeof(*plan));
}

static bool zend_mir_w05_source_block_to_mir(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id source_id, zend_mir_block_id *out)
{
	uint32_t index;
	uint32_t rank = 0;

	if (source == NULL || source->block_count == NULL
			|| source->block_at == NULL || out == NULL) {
		return false;
	}
	for (index = 0; index < source->block_count(source->context); index++) {
		zend_mir_source_block_ref block;
		if (!source->block_at(source->context, index, &block)) {
			return false;
		}
		if (block.id == source_id) {
			if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
				return false;
			}
			*out = rank;
			return true;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0) {
			rank++;
		}
	}
	return false;
}

static bool zend_mir_w05_argument_value(
	const zend_mir_lowering_context *context,
	const zend_mir_source_call_argument_ref *argument,
	zend_mir_value_id *out)
{
	zend_mir_value_fact_ref fact;
	zend_mir_value_id value;

	if (argument->value_ssa_variable_id != ZEND_MIR_ID_INVALID) {
		value = zend_mir_value_from_original_ssa(
			argument->value_ssa_variable_id);
	} else if (argument->source_operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value = zend_mir_value_from_synthetic(argument->source_operand.index);
	} else {
		return false;
	}
	if (!zend_mir_lowering_context_value_fact(context, value, &fact)
			|| fact.value_id != value
			|| !zend_mir_scalar_type_is_exact(fact.exact_type)
			|| (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
		return false;
	}
	*out = value;
	return true;
}

static bool zend_mir_w05_result_value(
	const zend_mir_lowering_context *context,
	const zend_mir_source_call_site_ref *site,
	zend_mir_value_id *out)
{
	zend_mir_value_fact_ref fact;
	zend_mir_value_id value;
	uint32_t result_flags;

	result_flags = site->flags
		& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
			| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
	if (result_flags == ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED) {
		if (zend_mir_id_is_valid(site->result_ssa_variable_id)) {
			return false;
		}
		*out = ZEND_MIR_ID_INVALID;
		return true;
	}
	if (result_flags != ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
			|| !zend_mir_id_is_valid(site->result_ssa_variable_id)) {
		return false;
	}
	value = zend_mir_value_from_original_ssa(
		site->result_ssa_variable_id);
	if (!zend_mir_lowering_context_value_fact(context, value, &fact)
			|| fact.value_id != value
			|| !zend_mir_scalar_type_is_exact(fact.exact_type)
			|| (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
		return false;
	}
	*out = value;
	return true;
}

static bool zend_mir_w05_is_call_init(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_NEW:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_w05_is_call_send(uint32_t opcode)
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
			return true;
		default:
			return false;
	}
}

static bool zend_mir_w05_is_supported_send(uint32_t opcode)
{
	return opcode == ZEND_SEND_VAL || opcode == ZEND_SEND_VAL_EX
		|| opcode == ZEND_SEND_VAR || opcode == ZEND_SEND_VAR_EX;
}

static bool zend_mir_w05_is_call_finish(uint32_t opcode)
{
	return opcode == ZEND_DO_UCALL || opcode == ZEND_DO_FCALL
		|| opcode == ZEND_DO_FCALL_BY_NAME || opcode == ZEND_DO_ICALL;
}

static zend_mir_lowering_diagnostic_code zend_mir_w05_source_sequence(
	const zend_mir_source_call_view *calls)
{
	zend_mir_source_call_site_id *stack = NULL;
	bool *seen_arguments = NULL;
	uint32_t source_count;
	uint32_t site_count;
	uint32_t argument_count;
	uint32_t stack_count = 0;
	uint32_t index;
	zend_mir_lowering_diagnostic_code result =
		ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;

	if (calls == NULL || calls->call_site_count == NULL
			|| calls->call_site_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->parameter_mode_count == NULL
			|| calls->parameter_mode_at == NULL
			|| calls->source_opcode_count == NULL
			|| calls->source_opcode_at == NULL) {
		return result;
	}
	source_count = calls->source_opcode_count(calls->context);
	site_count = calls->call_site_count(calls->context);
	argument_count = calls->call_argument_count(calls->context);
	if (source_count == 0 || site_count == 0) {
		return result;
	}
	stack = zend_mir_w05_calloc(
		site_count, sizeof(*stack), ZEND_MIR_W05_ALLOCATION_PLANNER);
	seen_arguments = zend_mir_w05_calloc(
		argument_count == 0 ? 1 : argument_count,
		sizeof(*seen_arguments), ZEND_MIR_W05_ALLOCATION_PLANNER);
	if (stack == NULL || seen_arguments == NULL) {
		result = ZEND_MIRL_W05_CALL_PLAN_FAILED;
		goto done;
	}
	for (index = 0; index < site_count; index++) {
		zend_mir_source_call_site_ref site;
		zend_mir_source_call_site_ref previous;
		if (!calls->call_site_at(calls->context, index, &site)
				|| site.id != index
				|| site.init_opline_index >= source_count
				|| site.do_opline_index >= source_count
				|| site.init_opline_index >= site.do_opline_index
				|| (index != 0
					&& (!calls->call_site_at(
							calls->context, index - 1, &previous)
						|| previous.init_opline_index
							>= site.init_opline_index))) {
			goto done;
		}
	}
	for (index = 0; index < argument_count; index++) {
		zend_mir_source_call_argument_ref argument;
		if (!calls->call_argument_at(calls->context, index, &argument)
				|| argument.id != index
				|| argument.call_site_id >= site_count
				|| argument.send_opline_index >= source_count) {
			goto done;
		}
	}
	for (index = 0; index < source_count; index++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_source_call_site_id init_id = ZEND_MIR_ID_INVALID;
		zend_mir_source_call_site_id finish_id = ZEND_MIR_ID_INVALID;
		zend_mir_source_call_argument_id argument_id = ZEND_MIR_ID_INVALID;
		uint32_t candidate;

		if (!calls->source_opcode_at(calls->context, index, &opcode)) {
			goto done;
		}
		for (candidate = 0; candidate < site_count; candidate++) {
			zend_mir_source_call_site_ref site;
			if (!calls->call_site_at(calls->context, candidate, &site)) {
				goto done;
			}
			if (site.init_opline_index == index) {
				if (zend_mir_id_is_valid(init_id)) {
					goto done;
				}
				init_id = site.id;
			}
			if (site.do_opline_index == index) {
				if (zend_mir_id_is_valid(finish_id)) {
					goto done;
				}
				finish_id = site.id;
			}
		}
		for (candidate = 0; candidate < argument_count; candidate++) {
			zend_mir_source_call_argument_ref argument_record;
			if (!calls->call_argument_at(
					calls->context, candidate, &argument_record)) {
				goto done;
			}
			if (argument_record.send_opline_index == index) {
				if (zend_mir_id_is_valid(argument_id)) {
					goto done;
				}
				argument_id = argument_record.id;
			}
		}
		if (zend_mir_id_is_valid(init_id)) {
			zend_mir_source_call_site_ref site_record;
			zend_mir_source_call_site_ref *site = &site_record;
			zend_mir_source_call_site_id parent =
				stack_count == 0
					? ZEND_MIR_ID_INVALID : stack[stack_count - 1];
			bool nested = stack_count != 0;
			if (!zend_mir_w05_is_call_init(opcode.zend_opcode_number)
					|| opcode.zend_opcode_number != ZEND_INIT_FCALL
					|| !calls->call_site_at(
						calls->context, init_id, site)
					|| site->parent_call_site_id != parent
					|| ((site->flags
							& ZEND_MIR_SOURCE_CALL_SITE_NESTED) != 0)
						!= nested
					|| stack_count >= site_count) {
				result = zend_mir_w05_is_call_init(
					opcode.zend_opcode_number)
					? ZEND_MIRL_W05_UNSUPPORTED_TARGET
					: ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			stack[stack_count++] = site->id;
		} else if (zend_mir_w05_is_call_init(opcode.zend_opcode_number)) {
			goto done;
		}
		if (zend_mir_id_is_valid(argument_id)) {
			zend_mir_source_call_argument_ref argument_record;
			zend_mir_source_call_argument_ref *argument = &argument_record;
			if (!zend_mir_w05_is_call_send(opcode.zend_opcode_number)
					|| !zend_mir_w05_is_supported_send(
						opcode.zend_opcode_number)
					|| !calls->call_argument_at(
						calls->context, argument_id, argument)
					|| seen_arguments[argument->id]
					|| argument->flags != 0
					|| stack_count == 0
					|| stack[stack_count - 1] != argument->call_site_id) {
				result = zend_mir_w05_is_call_send(
					opcode.zend_opcode_number)
					? ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT
					: ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			seen_arguments[argument->id] = true;
		} else if (zend_mir_w05_is_call_send(opcode.zend_opcode_number)) {
			goto done;
		}
		if (zend_mir_id_is_valid(finish_id)) {
			if (!zend_mir_w05_is_call_finish(opcode.zend_opcode_number)
					|| (opcode.zend_opcode_number != ZEND_DO_UCALL
						&& opcode.zend_opcode_number != ZEND_DO_FCALL)
					|| stack_count == 0
					|| stack[stack_count - 1] != finish_id) {
				result = zend_mir_w05_is_call_finish(
					opcode.zend_opcode_number)
					? ZEND_MIRL_W05_UNSUPPORTED_TARGET
					: ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			stack_count--;
		} else if (zend_mir_w05_is_call_finish(opcode.zend_opcode_number)) {
			goto done;
		}
	}
	if (stack_count != 0) {
		goto done;
	}
	for (index = 0; index < argument_count; index++) {
		if (!seen_arguments[index]) {
			goto done;
		}
	}
	result = ZEND_MIRL_OK;
done:
	free(seen_arguments);
	free(stack);
	return result;
}

static zend_mir_lowering_diagnostic_code zend_mir_w05_plan_calls(
	zend_mir_lowering_context *context,
	const zend_mir_source_call_view *calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_w05_plan *plan)
{
	uint32_t index;

	if (context == NULL || calls == NULL || resolver == NULL || plan == NULL
			|| calls->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| calls->call_site_count == NULL || calls->call_site_at == NULL
			|| calls->call_target_count == NULL || calls->call_target_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->source_opcode_count == NULL
			|| calls->source_opcode_at == NULL
			|| resolver->resolve_exact_direct_user == NULL) {
		return ZEND_MIRL_W05_CALL_PLAN_FAILED;
	}
	memset(plan, 0, sizeof(*plan));
	plan->site_count = calls->call_site_count(calls->context);
	plan->target_count = calls->call_target_count(calls->context);
	plan->argument_count = calls->call_argument_count(calls->context);
	if (plan->site_count == 0 || plan->target_count == 0) {
		return ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
	}
	if (plan->site_count > ZEND_MIR_ID_MAX / 4) {
		return ZEND_MIRL_W05_CALL_PLAN_FAILED;
	}
	plan->entries = zend_mir_w05_calloc(
		plan->site_count, sizeof(*plan->entries),
		ZEND_MIR_W05_ALLOCATION_PLANNER);
	plan->sites = zend_mir_w05_calloc(
		plan->site_count, sizeof(*plan->sites),
		ZEND_MIR_W05_ALLOCATION_PLANNER);
	plan->targets = zend_mir_w05_calloc(
		plan->target_count, sizeof(*plan->targets),
		ZEND_MIR_W05_ALLOCATION_TARGET_SNAPSHOT);
	plan->arguments = zend_mir_w05_calloc(
		plan->argument_count, sizeof(*plan->arguments),
		ZEND_MIR_W05_ALLOCATION_ARGUMENT_TABLE);
	plan->values = zend_mir_w05_calloc(
		plan->argument_count, sizeof(*plan->values),
		ZEND_MIR_W05_ALLOCATION_ARGUMENT_TABLE);
	plan->results = zend_mir_w05_calloc(
		plan->site_count, sizeof(*plan->results),
		ZEND_MIR_W05_ALLOCATION_PLANNER);
	plan->blocks = zend_mir_w05_calloc(
		plan->site_count, sizeof(*plan->blocks),
		ZEND_MIR_W05_ALLOCATION_PLANNER);
	if (plan->entries == NULL || plan->sites == NULL || plan->targets == NULL
			|| (plan->argument_count != 0
				&& (plan->arguments == NULL || plan->values == NULL))
			|| plan->results == NULL || plan->blocks == NULL) {
		return ZEND_MIRL_W05_CALL_PLAN_FAILED;
	}
	for (index = 0; index < plan->argument_count; index++) {
		if (!calls->call_argument_at(calls->context, index,
				&plan->arguments[index])) {
			return ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT;
		}
		if (plan->arguments[index].id != index
				|| plan->arguments[index].mode
					!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
				|| plan->arguments[index].flags != 0
				|| zend_mir_id_is_valid(
					plan->arguments[index].name_symbol_id)
				|| !zend_mir_w05_argument_value(context,
					&plan->arguments[index], &plan->values[index])) {
			return ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT;
		}
	}
	for (index = 0; index < plan->target_count; index++) {
		zend_mir_source_call_target_ref resolved;
		if (!calls->call_target_at(calls->context, index,
				&plan->targets[index])
				|| plan->targets[index].id != index
				|| !resolver->resolve_exact_direct_user(
					resolver->context, index, &resolved)
				|| !zend_mir_w05_target_equals(
					&resolved, &plan->targets[index])
				|| resolved.kind != ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				|| resolved.variadic || resolved.returns_by_reference
				|| !zend_mir_w05_target_parameter_modes_are_by_value(
					calls, &resolved)) {
			return ZEND_MIRL_W05_UNSUPPORTED_TARGET;
		}
	}
	for (index = 0; index < plan->site_count; index++) {
		zend_mir_source_call_site_ref *site = &plan->sites[index];
		zend_mir_call_plan_entry *entry = &plan->entries[index];
		uint32_t argument_index;
		if (!calls->call_site_at(calls->context, index, site)
				|| site->id != index || site->target_id >= plan->target_count
				|| site->argument_span.offset > plan->argument_count
				|| site->argument_span.count
					> plan->argument_count - site->argument_span.offset) {
			return ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
		}
		if ((site->flags & ZEND_MIR_SOURCE_CALL_SITE_PROTECTED) != 0) {
			return ZEND_MIRL_W05_PROTECTED_CALL;
		}
		if ((site->flags
				& (ZEND_MIR_SOURCE_CALL_SITE_NESTED
					| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR))
				== (ZEND_MIR_SOURCE_CALL_SITE_NESTED
					| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR)) {
			return ZEND_MIRL_W05_UNSUPPORTED_RESULT;
		}
		if (!zend_mir_w05_result_value(
				context, site, &plan->results[index])) {
			return ZEND_MIRL_W05_UNSUPPORTED_RESULT;
		}
		if (site->argument_span.count
				!= plan->targets[site->target_id].num_args) {
			return ZEND_MIRL_W05_ARGUMENT_COUNT_MISMATCH;
		}
		for (argument_index = 0;
				argument_index < site->argument_span.count;
				argument_index++) {
			const zend_mir_source_call_argument_ref *argument =
				&plan->arguments[site->argument_span.offset + argument_index];
			if (argument->call_site_id != site->id
					|| argument->ordinal != argument_index) {
				return ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE;
			}
		}
		if (!zend_mir_w05_source_block_to_mir(
				context->source, site->source_block_id,
				&plan->blocks[index])) {
			return ZEND_MIRL_W05_CALL_PLAN_FAILED;
		}
		entry->source_call_site_id = site->id;
		entry->decision = ZEND_MIR_CALL_PLAN_ACCEPTED;
		entry->diagnostic_code = ZEND_MIRL_OK;
		entry->argument_span = site->argument_span;
	}
	for (index = 0; index < plan->argument_count; index++) {
		uint32_t site_index;

		if (!zend_mir_id_is_valid(
				plan->arguments[index].value_ssa_variable_id)) {
			continue;
		}
		for (site_index = 0; site_index < plan->site_count; site_index++) {
			if (plan->sites[site_index].result_ssa_variable_id
					== plan->arguments[index].value_ssa_variable_id) {
				return ZEND_MIRL_W05_UNSUPPORTED_RESULT;
			}
		}
	}
	{
		zend_mir_lowering_diagnostic_code grammar =
			zend_mir_w05_source_sequence(calls);
		if (grammar != ZEND_MIRL_OK) {
			return grammar;
		}
	}
	plan->public_plan.entries = plan->entries;
	plan->public_plan.count = plan->site_count;
	plan->public_plan.complete = true;
	plan->public_plan.immutable = true;
	return ZEND_MIRL_OK;
}

static bool zend_mir_w05_emit_calls(
	const zend_mir_w05_plan *plan,
	const zend_mir_lowering_context *context,
	zend_mir_call_mutator *mutator)
{
	uint32_t index;

	if (plan == NULL || context == NULL || context->zend_source == NULL
			|| !zend_mir_id_is_valid(context->function_id)
			|| !zend_mir_id_is_valid(context->zend_source->op_array_id)
			|| mutator == NULL
			|| mutator->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| mutator->add_call_target == NULL
			|| mutator->add_call_argument == NULL
			|| mutator->add_call_continuation == NULL
			|| mutator->add_call_site == NULL
			|| mutator->commit_call_model == NULL) {
		return false;
	}
	for (index = 0; index < plan->target_count; index++) {
		const zend_mir_source_call_target_ref *source = &plan->targets[index];
		zend_mir_call_target_ref target;
		memset(&target, 0, sizeof(target));
		target.id = source->id;
		target.kind = ZEND_MIR_CALL_TARGET_DIRECT_USER;
		target.function_symbol_id = source->function_symbol_id;
		target.op_array_id = source->op_array_id;
		target.num_args = source->num_args;
		target.required_num_args = source->required_num_args;
		target.function_flags_snapshot = source->function_flags_snapshot;
		if (!mutator->add_call_target(mutator->context, &target)) {
			return false;
		}
	}
	for (index = 0; index < plan->argument_count; index++) {
		zend_mir_call_argument_ref argument;
		memset(&argument, 0, sizeof(argument));
		argument.id = index;
		argument.call_site_id = plan->arguments[index].call_site_id;
		argument.ordinal = plan->arguments[index].ordinal;
		argument.value_id = plan->values[index];
		argument.ownership = ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
		if (!mutator->add_call_argument(mutator->context, &argument)) {
			return false;
		}
	}
	for (index = 0; index < plan->site_count; index++) {
		const zend_mir_source_call_site_ref *source = &plan->sites[index];
		zend_mir_call_site_ref site;
		uint32_t continuation_index;
		memset(&site, 0, sizeof(site));
#ifdef ZEND_MIR_W05_TEST_FAULTS
		if (zend_mir_w05_test_fault_is(
				ZEND_MIR_W05_TEST_FAULT_FRAME_STATE)) {
			return false;
		}
#endif
		for (continuation_index = 0; continuation_index < 4;
				continuation_index++) {
			zend_mir_call_continuation_ref continuation;
			memset(&continuation, 0, sizeof(continuation));
			continuation.id = index * 4 + continuation_index;
			continuation.call_site_id = index;
			continuation.kind =
				(zend_mir_call_continuation_kind) continuation_index;
			continuation.block_id = continuation_index == 0
				? plan->blocks[index] : ZEND_MIR_ID_INVALID;
			continuation.semantic_debt = continuation_index == 1
				? ZEND_MIR_DEBT_CALL_EXCEPTION_PROPAGATION
				: continuation_index == 2
					? ZEND_MIR_DEBT_CALL_BAILOUT_REENTRY
					: continuation_index == 3
						? ZEND_MIR_DEBT_CALL_OBSERVER_INTEGRATION : 0;
			if (!mutator->add_call_continuation(
					mutator->context, &continuation)) {
				return false;
			}
		}
		site.id = index;
		site.source_call_site_id = source->id;
		site.instruction_id = source->do_opline_index;
		site.target_id = source->target_id;
		site.arguments = source->argument_span;
		site.result_id = plan->results[index];
		site.caller_frame.frame_state_id = ZEND_MIR_ID_INVALID;
		site.caller_frame.function_id = context->function_id;
		site.caller_frame.function_symbol_id = context->function_symbol_id;
		site.caller_frame.op_array_id = context->zend_source->op_array_id;
		site.caller_frame.pending_call_slot_id = ZEND_MIR_ID_INVALID;
		site.callee_entry_frame.frame_state_id = ZEND_MIR_ID_INVALID;
		site.callee_entry_frame.function_id = ZEND_MIR_ID_INVALID;
		site.callee_entry_frame.function_symbol_id =
			plan->targets[source->target_id].function_symbol_id;
		site.callee_entry_frame.op_array_id =
			plan->targets[source->target_id].op_array_id;
		site.callee_entry_frame.pending_call_slot_id = ZEND_MIR_ID_INVALID;
		site.continuations.offset = index * 4;
		site.continuations.count = 4;
		site.effects = W05_EFFECTS;
		site.reads = W05_READS;
		site.writes = W05_WRITES;
		site.barriers = W05_BARRIERS;
#ifdef ZEND_MIR_W05_TEST_FAULTS
		if (zend_mir_w05_test_fault_is(
				ZEND_MIR_W05_TEST_FAULT_CALL_RECORD)) {
			return false;
		}
#endif
		if (!mutator->add_call_site(mutator->context, &site)) {
			return false;
		}
	}
	return mutator->commit_call_model(mutator->context);
}

static bool zend_mir_w05_verify_emit(zend_mir_diagnostic_sink *diagnostics,
	zend_mir_verify_w05_code code, const char *token)
{
	zend_mir_diagnostic diagnostic;
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	(void) code;
	strncpy(diagnostic.message, token, sizeof(diagnostic.message) - 1);
	return zend_mir_diagnostic_sink_emit(diagnostics, &diagnostic);
}

static bool zend_mir_w05_span_equal(zend_mir_span left, zend_mir_span right)
{
	return left.offset == right.offset && left.count == right.count;
}

typedef struct _zend_mir_w05_fingerprint_digest {
	uint32_t words[4];
} zend_mir_w05_fingerprint_digest;

static zend_mir_w05_fingerprint_digest zend_mir_w05_fingerprint_init(void)
{
	zend_mir_w05_fingerprint_digest digest = {{
		UINT32_C(2166136261),
		UINT32_C(3339451269),
		UINT32_C(2593831049),
		UINT32_C(1268118805)
	}};
	return digest;
}

static zend_mir_w05_fingerprint_digest zend_mir_w05_fingerprint_mix(
	zend_mir_w05_fingerprint_digest digest, uint32_t value)
{
	static const uint32_t domains[4] = {
		UINT32_C(0x243f6a88), UINT32_C(0x85a308d3),
		UINT32_C(0x13198a2e), UINT32_C(0x03707344)
	};
	uint32_t word;
	uint32_t byte;
	for (word = 0; word < 4; word++) {
		uint32_t domain_value = value ^ domains[word];
		for (byte = 0; byte < 4; byte++) {
			digest.words[word] ^= domain_value & UINT32_C(0xff);
			digest.words[word] *= UINT32_C(16777619);
			domain_value >>= 8;
		}
	}
	return digest;
}

static zend_mir_w05_fingerprint_digest zend_mir_w05_fingerprint_mix_u64(
	zend_mir_w05_fingerprint_digest digest, uint64_t value)
{
	digest = zend_mir_w05_fingerprint_mix(digest, (uint32_t) value);
	return zend_mir_w05_fingerprint_mix(
		digest, (uint32_t) (value >> 32));
}

typedef struct _zend_mir_w05_fingerprint_writer {
	zend_mir_w05_fingerprint_digest digest;
} zend_mir_w05_fingerprint_writer;

static bool zend_mir_w05_fingerprint_write(
	void *context, const char *bytes, size_t length)
{
	zend_mir_w05_fingerprint_writer *writer = context;
	size_t index;

	if (writer == NULL || (bytes == NULL && length != 0)) {
		return false;
	}
	for (index = 0; index < length; index++) {
		writer->digest = zend_mir_w05_fingerprint_mix(
			writer->digest, (unsigned char) bytes[index]);
	}
	return true;
}

static zend_mir_w05_fingerprint_digest
zend_mir_w05_fingerprint_source_operand(
	zend_mir_w05_fingerprint_digest digest,
	const zend_mir_source_operand_ref *operand)
{
	digest = zend_mir_w05_fingerprint_mix(
		digest, (uint32_t) operand->kind);
	digest = zend_mir_w05_fingerprint_mix(
		digest, (uint32_t) operand->slot_kind);
	digest = zend_mir_w05_fingerprint_mix(digest, operand->index);
	return zend_mir_w05_fingerprint_mix(
		digest, operand->ssa_variable_id);
}

static bool zend_mir_w05_build_fingerprints(
	const zend_mir_view *view,
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics,
	uint32_t module_fingerprint[4],
	uint32_t source_fingerprint[4])
{
	zend_mir_w05_fingerprint_writer module_writer;
	zend_mir_text_writer writer;
	zend_mir_w05_fingerprint_digest source_seed =
		zend_mir_w05_fingerprint_init();
	uint32_t index;

	if (view == NULL || source == NULL || source_calls == NULL || calls == NULL
			|| diagnostics == NULL
			|| source->opcode_count == NULL || source->opcode_at == NULL
			|| source->ssa_count == NULL || source->ssa_at == NULL
			|| source->ssa_use_count == NULL || source->ssa_use_at == NULL
			|| source->ssa_def_count == NULL || source->ssa_def_at == NULL
			|| source->literal_count == NULL || source->literal_at == NULL
			|| source->block_count == NULL || source->block_at == NULL
			|| source->edge_count == NULL || source->edge_at == NULL
			|| source->phi_count == NULL || source->phi_at == NULL
			|| source->phi_input_count == NULL
			|| source->phi_input_at == NULL
			|| source_calls->source_opcode_count == NULL
			|| source_calls->source_opcode_at == NULL
			|| source_calls->call_site_count == NULL
			|| source_calls->call_site_at == NULL
			|| source_calls->call_target_count == NULL
			|| source_calls->call_target_at == NULL
			|| source_calls->call_argument_count == NULL
			|| source_calls->call_argument_at == NULL
			|| source_calls->parameter_mode_count == NULL
			|| source_calls->parameter_mode_at == NULL
			|| calls->call_site_count == NULL
			|| calls->call_target_count == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_continuation_count == NULL) {
		return false;
	}
	module_writer.digest = zend_mir_w05_fingerprint_init();
	writer.context = &module_writer;
	writer.write = zend_mir_w05_fingerprint_write;
	if (!zend_mir_dump_text(view, &writer, diagnostics)) {
		return false;
	}

#define W05_MIX(record, field) \
	source_seed = zend_mir_w05_fingerprint_mix_u64( \
		source_seed, (uint64_t) (record).field)
#define W05_MIX_OPERAND(record, field) \
	source_seed = zend_mir_w05_fingerprint_source_operand( \
		source_seed, &(record).field)

	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->opcode_count(source->context));
	for (index = 0; index < source->opcode_count(source->context); index++) {
		zend_mir_source_opcode_ref record;
		if (!source->opcode_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, opline_index);
		W05_MIX(record, zend_opcode_number);
		W05_MIX_OPERAND(record, op1);
		W05_MIX_OPERAND(record, op2);
		W05_MIX_OPERAND(record, result);
		W05_MIX(record, extended_value);
		W05_MIX(record, source_position_id);
		W05_MIX(record, block_id);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->ssa_count(source->context));
	for (index = 0; index < source->ssa_count(source->context); index++) {
		zend_mir_source_ssa_ref record;
		if (!source->ssa_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, ssa_variable_id);
		W05_MIX(record, definition_opline_index);
		W05_MIX(record, source_slot);
		W05_MIX(record, source_slot_kind);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->ssa_use_count(source->context));
	for (index = 0; index < source->ssa_use_count(source->context); index++) {
		zend_mir_source_ssa_use_ref record;
		if (!source->ssa_use_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, ssa_variable_id);
		W05_MIX(record, opline_index);
		W05_MIX(record, operand_index);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->ssa_def_count(source->context));
	for (index = 0; index < source->ssa_def_count(source->context); index++) {
		zend_mir_source_ssa_def_ref record;
		if (!source->ssa_def_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, ssa_variable_id);
		W05_MIX(record, opline_index);
		W05_MIX_OPERAND(record, destination);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->literal_count(source->context));
	for (index = 0; index < source->literal_count(source->context); index++) {
		zend_mir_source_literal_ref record;
		if (!source->literal_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, literal_index);
		W05_MIX(record, kind);
		W05_MIX(record, payload_bits);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->block_count(source->context));
	for (index = 0; index < source->block_count(source->context); index++) {
		zend_mir_source_block_ref record;
		if (!source->block_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, first_opcode_ordinal);
		W05_MIX(record, opcode_count);
		W05_MIX(record, flags);
		W05_MIX(record, immediate_dominator);
		W05_MIX(record, loop_header);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->edge_count(source->context));
	for (index = 0; index < source->edge_count(source->context); index++) {
		zend_mir_source_edge_ref record;
		if (!source->edge_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, from_block_id);
		W05_MIX(record, to_block_id);
		W05_MIX(record, successor_index);
		W05_MIX(record, predecessor_index);
		W05_MIX(record, flags);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->phi_count(source->context));
	for (index = 0; index < source->phi_count(source->context); index++) {
		zend_mir_source_phi_ref record;
		if (!source->phi_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, block_id);
		W05_MIX(record, result_ssa_variable_id);
		W05_MIX(record, source_slot_kind);
		W05_MIX(record, source_slot_index);
		W05_MIX(record, kind);
		W05_MIX(record, constraint.type_mask);
		W05_MIX(record, constraint.range_min);
		W05_MIX(record, constraint.range_max);
		W05_MIX(record, constraint.min_ssa_variable_id);
		W05_MIX(record, constraint.max_ssa_variable_id);
		W05_MIX(record, constraint.flags);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source->phi_input_count(source->context));
	for (index = 0;
			index < source->phi_input_count(source->context); index++) {
		zend_mir_source_phi_input_ref record;
		if (!source->phi_input_at(source->context, index, &record)) {
			return false;
		}
		W05_MIX(record, phi_id);
		W05_MIX(record, input_index);
		W05_MIX(record, predecessor_block_id);
		W05_MIX(record, source_ssa_variable_id);
	}

	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source_calls->source_opcode_count(source_calls->context));
	for (index = 0;
			index < source_calls->source_opcode_count(source_calls->context);
			index++) {
		zend_mir_source_opcode_ref record;
		if (!source_calls->source_opcode_at(
				source_calls->context, index, &record)) {
			return false;
		}
		W05_MIX(record, opline_index);
		W05_MIX(record, zend_opcode_number);
		W05_MIX_OPERAND(record, op1);
		W05_MIX_OPERAND(record, op2);
		W05_MIX_OPERAND(record, result);
		W05_MIX(record, extended_value);
		W05_MIX(record, source_position_id);
		W05_MIX(record, block_id);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source_calls->call_site_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_site_count(source_calls->context);
			index++) {
		zend_mir_source_call_site_ref record;
		if (!source_calls->call_site_at(
				source_calls->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, parent_call_site_id);
		W05_MIX(record, init_opline_index);
		W05_MIX(record, do_opline_index);
		W05_MIX(record, source_block_id);
		W05_MIX(record, target_id);
		W05_MIX(record, argument_span.offset);
		W05_MIX(record, argument_span.count);
		W05_MIX(record, result_ssa_variable_id);
		W05_MIX(record, flags);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source_calls->call_target_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_target_count(source_calls->context);
			index++) {
		zend_mir_source_call_target_ref record;
		if (!source_calls->call_target_at(
				source_calls->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, kind);
		W05_MIX(record, function_symbol_id);
		W05_MIX(record, op_array_id);
		W05_MIX(record, num_args);
		W05_MIX(record, required_num_args);
		W05_MIX(record, function_flags_snapshot);
		W05_MIX(record, parameter_modes.offset);
		W05_MIX(record, parameter_modes.count);
		W05_MIX(record, variadic);
		W05_MIX(record, returns_by_reference);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source_calls->call_argument_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_argument_count(source_calls->context);
			index++) {
		zend_mir_source_call_argument_ref record;
		if (!source_calls->call_argument_at(
				source_calls->context, index, &record)) {
			return false;
		}
		W05_MIX(record, id);
		W05_MIX(record, call_site_id);
		W05_MIX(record, send_opline_index);
		W05_MIX(record, ordinal);
		W05_MIX(record, name_symbol_id);
		W05_MIX(record, mode);
		W05_MIX(record, flags);
		W05_MIX(record, value_ssa_variable_id);
		W05_MIX_OPERAND(record, source_operand);
	}
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, source_calls->parameter_mode_count(source_calls->context));
	for (index = 0;
			index < source_calls->parameter_mode_count(source_calls->context);
			index++) {
		zend_mir_source_parameter_mode_ref record;
		if (!source_calls->parameter_mode_at(
				source_calls->context, index, &record)) {
			return false;
		}
		W05_MIX(record, target_id);
		W05_MIX(record, ordinal);
		W05_MIX(record, mode);
	}
#undef W05_MIX_OPERAND
#undef W05_MIX

	module_writer.digest = zend_mir_w05_fingerprint_mix(
		module_writer.digest, ZEND_MIR_W05_CONTRACT_VERSION);
	source_seed = zend_mir_w05_fingerprint_mix(
		source_seed, ZEND_MIR_W05_CONTRACT_VERSION);
	memcpy(module_fingerprint, module_writer.digest.words,
		sizeof(module_writer.digest.words));
	memcpy(source_fingerprint, source_seed.words,
		sizeof(source_seed.words));
	return true;
}

static bool zend_mir_w05_words_equal(
	const uint32_t left[4], const uint32_t right[4])
{
	return memcmp(left, right, 4 * sizeof(uint32_t)) == 0;
}

static bool zend_mir_w05_verify_scalar_result(
	const zend_mir_view *view, zend_mir_value_id value_id,
	zend_mir_representation *representation_out)
{
	zend_mir_value_record value;
	zend_mir_value_fact_ref fact;
	bool found_value = false;
	bool found_fact = false;
	uint32_t index;

	for (index = 0; index < view->value_count(view->context); index++) {
		if (!view->value_at(view->context, index, &value)) {
			return false;
		}
		if (value.id == value_id) {
			found_value = true;
			break;
		}
	}
	for (index = 0; index < view->value_fact_count(view->context); index++) {
		if (!view->value_fact_at(view->context, index, &fact)) {
			return false;
		}
		if (fact.value_id == value_id) {
			found_fact = true;
			break;
		}
	}
	if (!found_value || !found_fact
			|| !zend_mir_scalar_type_is_exact(fact.exact_type)
			|| (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
		return false;
	}
	*representation_out = value.representation;
	return true;
}

static bool zend_mir_w05_verify_source_order(
	const zend_mir_view *view,
	const zend_mir_instruction_record *call,
	uint32_t do_opline_index)
{
	uint32_t index;

	if (call->source_position_id != do_opline_index) {
		return false;
	}
	for (index = 0; index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record candidate;
		if (!view->instruction_at(view->context, index, &candidate)) {
			return false;
		}
		if (candidate.block_id != call->block_id
				|| !zend_mir_id_is_valid(candidate.source_position_id)
				|| candidate.id == call->id) {
			continue;
		}
		if ((candidate.id < call->id
					&& candidate.source_position_id > do_opline_index)
				|| (candidate.id > call->id
					&& candidate.source_position_id < do_opline_index)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w05_verify_frame_shape(
	const zend_mir_frame_state_ref *frame,
	const zend_mir_call_frame_descriptor *descriptor,
	zend_mir_frame_state_id parent_id,
	uint32_t opline_index,
	zend_mir_safepoint_class safepoint)
{
	return frame->id == descriptor->frame_state_id
		&& frame->function_id == descriptor->function_id
		&& frame->parent_id == parent_id
		&& frame->function_kind == ZEND_MIR_FUNCTION_KIND_USER
		&& opline_index != UINT32_MAX
		&& frame->opline_index == opline_index
		&& frame->opline_phase == ZEND_MIR_OPLINE_PHASE_BEFORE
		&& zend_mir_w05_span_equal(frame->slots, descriptor->slots)
		&& frame->roots.count == 0
		&& frame->cleanup_obligations.count == 0
		&& frame->return_continuation.kind
			== ZEND_MIR_CONTINUATION_KIND_NATIVE
		&& frame->return_continuation.frame_state_id == ZEND_MIR_ID_INVALID
		&& frame->return_continuation.opline_index == opline_index + 1
		&& frame->exception_continuation.kind
			== ZEND_MIR_CONTINUATION_KIND_ZEND_EXCEPTION
		&& frame->exception_continuation.frame_state_id == ZEND_MIR_ID_INVALID
		&& frame->exception_continuation.opline_index == opline_index
		&& frame->bailout_continuation.kind
			== ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT
		&& frame->bailout_continuation.frame_state_id == ZEND_MIR_ID_INVALID
		&& frame->bailout_continuation.opline_index == opline_index
		&& frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
		&& frame->suspend_state_id == ZEND_MIR_ID_INVALID
		&& frame->code_version_id == 1
		&& !frame->resume.allowed
		&& frame->resume.entry_kind == ZEND_MIR_RESUME_ENTRY_KIND_NONE
		&& frame->resume.resume_id == ZEND_MIR_ID_INVALID
		&& frame->resume.code_version_id == ZEND_MIR_ID_INVALID
		&& frame->resume.target_opline_index == ZEND_MIR_ID_INVALID
		&& frame->safepoint_class == safepoint
		&& frame->canonical;
}

bool zend_mir_verify_w05_calls(
	const zend_mir_view *view,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;
	uint32_t frame_slot_count;
	uint32_t source_argument_count;
	uint32_t source_site_count;
	uint32_t source_target_count;

	if (view == NULL || source_calls == NULL || calls == NULL
			|| calls->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| source_calls->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| source_calls->call_site_count == NULL
			|| source_calls->call_site_at == NULL
			|| source_calls->call_target_count == NULL
			|| source_calls->call_target_at == NULL
			|| source_calls->call_argument_count == NULL
			|| source_calls->call_argument_at == NULL
			|| source_calls->parameter_mode_count == NULL
			|| source_calls->parameter_mode_at == NULL
			|| source_calls->source_opcode_count == NULL
			|| source_calls->source_opcode_at == NULL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->call_target_count == NULL || calls->call_target_at == NULL
			|| calls->call_continuation_count == NULL
			|| calls->call_continuation_at == NULL
			|| view->function_count == NULL
			|| view->function_at == NULL
			|| view->instruction_count == NULL
			|| view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->instruction_operand_at == NULL
			|| view->value_count == NULL || view->value_at == NULL
			|| view->value_fact_count == NULL || view->value_fact_at == NULL
			|| view->frame_state_count == NULL
			|| view->frame_state_at == NULL
			|| view->frame_slot_count == NULL
			|| view->frame_slot_at == NULL) {
		zend_mir_w05_verify_emit(diagnostics,
			ZEND_MIR_VERIFY_W05_SITE_MISMATCH,
			ZEND_MIRV_TOKEN_W05_SITE_MISMATCH);
		return false;
	}
	source_site_count = source_calls->call_site_count(source_calls->context);
	source_target_count =
		source_calls->call_target_count(source_calls->context);
	source_argument_count =
		source_calls->call_argument_count(source_calls->context);
	frame_slot_count = view->frame_slot_count(view->context);
	if (zend_mir_w05_source_sequence(source_calls) != ZEND_MIRL_OK
			|| source_site_count == 0
			|| source_site_count > ZEND_MIR_ID_MAX / 4
			|| calls->call_site_count(calls->context) != source_site_count
			|| calls->call_argument_count(calls->context)
				!= source_argument_count
			|| calls->call_target_count(calls->context)
				!= source_target_count) {
		zend_mir_w05_verify_emit(diagnostics,
			ZEND_MIR_VERIFY_W05_SITE_MISMATCH,
			ZEND_MIRV_TOKEN_W05_SITE_MISMATCH);
		return false;
	}
	for (index = 0; index < source_target_count; index++) {
		zend_mir_source_call_target_ref source;
		zend_mir_call_target_ref target;
		if (!source_calls->call_target_at(
					source_calls->context, index, &source)
				|| !calls->call_target_at(calls->context, index, &target)
				|| source.id != index || target.id != index
				|| source.kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				|| target.kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
				|| target.function_symbol_id != source.function_symbol_id
				|| target.op_array_id != source.op_array_id
				|| target.num_args != source.num_args
				|| target.required_num_args != source.required_num_args
				|| target.function_flags_snapshot
					!= source.function_flags_snapshot
				|| source.variadic || source.returns_by_reference
				|| !zend_mir_w05_target_parameter_modes_are_by_value(
					source_calls, &source)) {
			zend_mir_w05_verify_emit(diagnostics,
				ZEND_MIR_VERIFY_W05_TARGET_MISMATCH,
				ZEND_MIRV_TOKEN_W05_TARGET_MISMATCH);
			return false;
		}
	}
	for (index = 0; index < source_site_count; index++) {
		zend_mir_source_call_site_ref source;
		zend_mir_call_site_ref site;
		zend_mir_instruction_record instruction;
		zend_mir_source_call_target_ref source_target;
		zend_mir_function_record caller_function;
		zend_mir_frame_state_ref caller_frame;
		zend_mir_call_continuation_ref normal_continuation;
		zend_mir_value_id expected_result;
		zend_mir_representation expected_representation;
		uint32_t result_flags;
		uint32_t operand;
		if (!source_calls->call_site_at(
				source_calls->context, index, &source)) {
			zend_mir_w05_verify_emit(diagnostics,
				ZEND_MIR_VERIFY_W05_SITE_MISMATCH,
				ZEND_MIRV_TOKEN_W05_SITE_MISMATCH);
			return false;
		}
		result_flags = source.flags
			& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
				| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
		expected_result =
			result_flags == ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
				&& zend_mir_id_is_valid(source.result_ssa_variable_id)
			? zend_mir_value_from_original_ssa(
				source.result_ssa_variable_id)
			: ZEND_MIR_ID_INVALID;
		expected_representation = ZEND_MIR_REPRESENTATION_VOID;
		if (!calls->call_site_at(calls->context, index, &site)
				|| source.target_id >= source_target_count
				|| !source_calls->call_target_at(
					source_calls->context, source.target_id, &source_target)
				|| site.id != index || site.source_call_site_id != source.id
				|| site.target_id != source.target_id
				|| source.argument_span.offset > source_argument_count
				|| source.argument_span.count
					> source_argument_count - source.argument_span.offset
				|| site.arguments.offset != source.argument_span.offset
				|| site.arguments.count != source.argument_span.count
				|| site.arguments.offset
					> calls->call_argument_count(calls->context)
				|| site.arguments.count
					> calls->call_argument_count(calls->context)
						- site.arguments.offset
				|| site.continuations.offset != index * 4
				|| site.continuations.count != 4
				|| site.effects != W05_EFFECTS || site.reads != W05_READS
				|| site.writes != W05_WRITES || site.barriers != W05_BARRIERS
				|| site.instruction_id >= view->instruction_count(view->context)
				|| !view->instruction_at(view->context, site.instruction_id,
					&instruction)
				|| instruction.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| (result_flags
						!= ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
					&& result_flags
						!= ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR)
				|| (result_flags
						== ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
					&& zend_mir_id_is_valid(
						source.result_ssa_variable_id))
				|| (result_flags
						== ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
					&& (!zend_mir_id_is_valid(
							source.result_ssa_variable_id)
						|| !zend_mir_w05_verify_scalar_result(
							view, expected_result,
							&expected_representation)))
				|| site.result_id != expected_result
				|| instruction.result_id != expected_result
				|| instruction.representation != expected_representation
				|| !zend_mir_w05_verify_source_order(
					view, &instruction, source.do_opline_index)
				|| view->instruction_operand_count(
					view->context, instruction.id) != site.arguments.count
				|| !calls->call_continuation_at(calls->context,
					site.continuations.offset, &normal_continuation)
				|| normal_continuation.call_site_id != site.id
				|| normal_continuation.kind
					!= ZEND_MIR_CALL_CONTINUATION_NORMAL
				|| normal_continuation.block_id != instruction.block_id) {
			zend_mir_w05_verify_emit(diagnostics,
				ZEND_MIR_VERIFY_W05_SITE_MISMATCH,
				ZEND_MIRV_TOKEN_W05_SITE_MISMATCH);
			return false;
		}
		for (operand = 0; operand < site.arguments.count; operand++) {
			zend_mir_source_call_argument_ref source_argument;
			zend_mir_call_argument_ref argument;
			zend_mir_value_id value;
			zend_mir_value_id expected_value;
			if (!source_calls->call_argument_at(
					source_calls->context,
					source.argument_span.offset + operand,
					&source_argument)) {
				zend_mir_w05_verify_emit(diagnostics,
					ZEND_MIR_VERIFY_W05_ARGUMENT_MISMATCH,
					ZEND_MIRV_TOKEN_W05_ARGUMENT_MISMATCH);
				return false;
			}
			if (zend_mir_id_is_valid(
					source_argument.value_ssa_variable_id)) {
				expected_value = zend_mir_value_from_original_ssa(
					source_argument.value_ssa_variable_id);
			} else if (source_argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				expected_value = zend_mir_value_from_synthetic(
					source_argument.source_operand.index);
			} else {
				zend_mir_w05_verify_emit(diagnostics,
					ZEND_MIR_VERIFY_W05_ARGUMENT_MISMATCH,
					ZEND_MIRV_TOKEN_W05_ARGUMENT_MISMATCH);
				return false;
			}
			if (!calls->call_argument_at(calls->context,
					site.arguments.offset + operand, &argument)
					|| source_argument.id
						!= source.argument_span.offset + operand
					|| source_argument.call_site_id != source.id
					|| source_argument.ordinal != operand
					|| source_argument.mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					|| source_argument.flags != 0
					|| zend_mir_id_is_valid(
						source_argument.name_symbol_id)
					|| argument.id != source_argument.id
					|| argument.call_site_id != site.id
					|| argument.ordinal != operand
					|| argument.value_id != expected_value
					|| argument.ownership
						!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
					|| !view->instruction_operand_at(view->context,
						instruction.id, operand, &value)
					|| value != argument.value_id) {
				zend_mir_w05_verify_emit(diagnostics,
					ZEND_MIR_VERIFY_W05_ARGUMENT_MISMATCH,
					ZEND_MIRV_TOKEN_W05_ARGUMENT_MISMATCH);
				return false;
			}
		}
		if (!zend_mir_id_is_valid(site.caller_frame.op_array_id)
				|| site.callee_entry_frame.op_array_id
					!= source_target.op_array_id
				|| site.callee_entry_frame.function_symbol_id
					!= source_target.function_symbol_id
				|| site.caller_frame.function_id
					>= view->function_count(view->context)
				|| !view->function_at(
					view->context, site.caller_frame.function_id,
					&caller_function)
				|| caller_function.id != site.caller_frame.function_id
				|| caller_function.symbol_id
					!= site.caller_frame.function_symbol_id
				|| zend_mir_id_is_valid(
					site.callee_entry_frame.function_id)
				|| zend_mir_id_is_valid(
					site.callee_entry_frame.frame_state_id)
				|| site.arguments.count == UINT32_MAX
				|| site.caller_frame.slots.count
					!= site.arguments.count + 1
				|| site.callee_entry_frame.slots.count
					!= site.arguments.count
				|| site.caller_frame.slots.offset > frame_slot_count
				|| site.caller_frame.slots.count
					> frame_slot_count - site.caller_frame.slots.offset
				|| site.callee_entry_frame.slots.offset > frame_slot_count
				|| site.callee_entry_frame.slots.count
					> frame_slot_count
						- site.callee_entry_frame.slots.offset
				|| !zend_mir_id_is_valid(
					site.caller_frame.pending_call_slot_id)
				|| zend_mir_id_is_valid(
					site.callee_entry_frame.pending_call_slot_id)
				|| site.caller_frame.frame_state_id
					>= view->frame_state_count(view->context)
				|| !view->frame_state_at(
					view->context,
					site.caller_frame.frame_state_id, &caller_frame)
				|| !zend_mir_w05_verify_frame_shape(
					&caller_frame, &site.caller_frame,
					ZEND_MIR_ID_INVALID, source.do_opline_index,
					ZEND_MIR_SAFEPOINT_CLASS_USER_CALL)
				|| instruction.frame_state_id != caller_frame.id) {
			zend_mir_w05_verify_emit(diagnostics,
				ZEND_MIR_VERIFY_W05_FRAME_MISMATCH,
				ZEND_MIRV_TOKEN_W05_FRAME_MISMATCH);
			return false;
		}
		for (operand = 0; operand < site.arguments.count; operand++) {
			zend_mir_call_argument_ref argument;
			zend_mir_frame_slot_ref caller_slot;
			zend_mir_frame_slot_ref callee_slot;
			if (!calls->call_argument_at(
					calls->context, site.arguments.offset + operand,
					&argument)
					|| !view->frame_slot_at(
						view->context,
						site.caller_frame.slots.offset + operand,
						&caller_slot)
					|| !view->frame_slot_at(
						view->context,
						site.callee_entry_frame.slots.offset + operand,
						&callee_slot)
					|| caller_slot.value_id != argument.value_id
					|| caller_slot.index != operand
					|| caller_slot.kind
						!= ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT
					|| caller_slot.representation
						!= ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL
					|| caller_slot.materialization
						!= ZEND_MIR_MATERIALIZATION_MATERIALIZED
					|| caller_slot.ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_CALLER_OWNED
					|| caller_slot.rooted
					|| caller_slot.cleanup_required
					|| callee_slot.value_id != argument.value_id
					|| callee_slot.index != operand
					|| callee_slot.kind
						!= ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT
					|| callee_slot.representation
						!= ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL
					|| callee_slot.materialization
						!= ZEND_MIR_MATERIALIZATION_MATERIALIZED
					|| callee_slot.ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED
					|| callee_slot.rooted
					|| callee_slot.cleanup_required) {
				zend_mir_w05_verify_emit(diagnostics,
					ZEND_MIR_VERIFY_W05_FRAME_MISMATCH,
					ZEND_MIRV_TOKEN_W05_FRAME_MISMATCH);
				return false;
			}
		}
		{
			zend_mir_frame_slot_ref pending;
			if (!view->frame_slot_at(
					view->context,
					site.caller_frame.slots.offset
						+ site.arguments.count,
					&pending)
					|| pending.slot_id
						!= site.caller_frame.pending_call_slot_id
					|| zend_mir_id_is_valid(pending.value_id)
					|| pending.index != 0
					|| pending.kind
						!= ZEND_MIR_FRAME_SLOT_KIND_PENDING_CALL
					|| pending.representation
						!= ZEND_MIR_FRAME_SLOT_REPRESENTATION_EXECUTE_DATA_FIELD
					|| pending.materialization != ZEND_MIR_MATERIALIZATION_UNDEF
					|| pending.ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED
					|| pending.rooted || pending.cleanup_required) {
				zend_mir_w05_verify_emit(diagnostics,
					ZEND_MIR_VERIFY_W05_FRAME_MISMATCH,
					ZEND_MIRV_TOKEN_W05_FRAME_MISMATCH);
				return false;
			}
		}
	}
	if (calls->call_continuation_count(calls->context)
			!= source_site_count * 4) {
		zend_mir_w05_verify_emit(diagnostics,
			ZEND_MIR_VERIFY_W05_CONTINUATION_MISMATCH,
			ZEND_MIRV_TOKEN_W05_CONTINUATION_MISMATCH);
		return false;
	}
	for (index = 0; index < source_site_count * 4; index++) {
		zend_mir_call_continuation_ref continuation;
		if (!calls->call_continuation_at(calls->context, index, &continuation)
				|| continuation.id != index
				|| continuation.kind
					!= (zend_mir_call_continuation_kind) (index % 4)
				|| continuation.call_site_id != index / 4
				|| continuation.semantic_debt
					!= ((index % 4) == 1
						? ZEND_MIR_DEBT_CALL_EXCEPTION_PROPAGATION
						: (index % 4) == 2
							? ZEND_MIR_DEBT_CALL_BAILOUT_REENTRY
							: (index % 4) == 3
								? ZEND_MIR_DEBT_CALL_OBSERVER_INTEGRATION
								: 0)
				|| ((index % 4) == 0)
					!= zend_mir_id_is_valid(continuation.block_id)) {
			zend_mir_w05_verify_emit(diagnostics,
				ZEND_MIR_VERIFY_W05_CONTINUATION_MISMATCH,
				ZEND_MIRV_TOKEN_W05_CONTINUATION_MISMATCH);
			return false;
		}
	}
	return true;
}

static bool zend_mir_w05_find_function(
	const zend_mir_view *view, zend_mir_function_id id,
	zend_mir_function_record *out)
{
	uint32_t index;
	for (index = 0; index < view->function_count(view->context); index++) {
		zend_mir_function_record record;
		if (!view->function_at(view->context, index, &record)) {
			return false;
		}
		if (record.id == id) {
			if (out != NULL) {
				*out = record;
			}
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_find_block(
	const zend_mir_view *view, zend_mir_block_id id,
	zend_mir_block_record *out)
{
	uint32_t index;
	for (index = 0; index < view->block_count(view->context); index++) {
		zend_mir_block_record record;
		if (!view->block_at(view->context, index, &record)) {
			return false;
		}
		if (record.id == id) {
			if (out != NULL) {
				*out = record;
			}
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_find_value(
	const zend_mir_view *view, zend_mir_value_id id)
{
	uint32_t index;
	for (index = 0; index < view->value_count(view->context); index++) {
		zend_mir_value_record record;
		if (!view->value_at(view->context, index, &record)) {
			return false;
		}
		if (record.id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_find_frame_state(
	const zend_mir_view *view, zend_mir_frame_state_id id)
{
	uint32_t index;
	for (index = 0; index < view->frame_state_count(view->context); index++) {
		zend_mir_frame_state_ref record;
		if (!view->frame_state_at(view->context, index, &record)) {
			return false;
		}
		if (record.id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_find_source_position(
	const zend_mir_view *view, zend_mir_source_position_id id)
{
	uint32_t index;
	for (index = 0;
		index < view->source_position_count(view->context); index++) {
		zend_mir_source_position_ref record;
		if (!view->source_position_at(
				view->context, index, &record)) {
			return false;
		}
		if (record.id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_verify_final_structural(
	const zend_mir_view *view, zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (zend_mir_w05_test_fault_is(
			ZEND_MIR_W05_TEST_FAULT_STRUCTURAL_VERIFIER)
			|| view == NULL
			|| view->function_count == NULL || view->function_at == NULL
			|| view->block_count == NULL || view->block_at == NULL
			|| view->instruction_count == NULL
			|| view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->instruction_operand_at == NULL
			|| view->value_count == NULL || view->value_at == NULL
			|| view->frame_state_count == NULL
			|| view->frame_state_at == NULL
			|| view->source_position_count == NULL
			|| view->source_position_at == NULL) {
		goto failure;
	}
	for (index = 0; index < view->function_count(view->context); index++) {
		zend_mir_function_record function;
		zend_mir_block_record entry;
		if (!view->function_at(view->context, index, &function)
				|| !zend_mir_w05_find_block(
					view, function.entry_block_id, &entry)
				|| entry.function_id != function.id) {
			goto failure;
		}
	}
	for (index = 0; index < view->block_count(view->context); index++) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, index, &block)
				|| !zend_mir_w05_find_function(
					view, block.function_id, NULL)) {
			goto failure;
		}
	}
	for (index = 0;
		index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record instruction;
		uint32_t operand_index;
		uint32_t operand_count;
		if (!view->instruction_at(view->context, index, &instruction)
				|| !zend_mir_w05_find_block(
					view, instruction.block_id, NULL)
				|| (uint32_t) instruction.opcode
					>= ZEND_MIR_W05_OPCODE_COUNT
				|| (uint32_t) instruction.representation
					>= ZEND_MIR_REPRESENTATION_COUNT
				|| (zend_mir_id_is_valid(instruction.result_id)
					&& !zend_mir_w05_find_value(
						view, instruction.result_id))
				|| (zend_mir_id_is_valid(instruction.frame_state_id)
					&& !zend_mir_w05_find_frame_state(
						view, instruction.frame_state_id))
				|| (zend_mir_id_is_valid(
						instruction.source_position_id)
					&& !zend_mir_w05_find_source_position(
						view, instruction.source_position_id))) {
			goto failure;
		}
		operand_count = view->instruction_operand_count(
			view->context, instruction.id);
		if (operand_count > view->value_count(view->context)) {
			goto failure;
		}
		for (operand_index = 0;
			operand_index < operand_count; operand_index++) {
			zend_mir_value_id operand;
			if (!view->instruction_operand_at(
					view->context, instruction.id,
					operand_index, &operand)
					|| !zend_mir_w05_find_value(view, operand)) {
				goto failure;
			}
		}
	}
	return true;

failure:
	zend_mir_w05_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_W05_SITE_MISMATCH,
		ZEND_MIRV_TOKEN_W05_SITE_MISMATCH);
	return false;
}

static bool zend_mir_w05_verify_final_scalar(
	const zend_mir_view *view, const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (zend_mir_w05_test_fault_is(
			ZEND_MIR_W05_TEST_FAULT_SCALAR_VERIFIER)
			|| view == NULL || calls == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->call_site_count == NULL
			|| calls->call_site_at == NULL) {
		goto failure;
	}
	for (index = 0;
		index < calls->call_argument_count(calls->context); index++) {
		zend_mir_call_argument_ref argument;
		zend_mir_representation representation;
		if (!calls->call_argument_at(
				calls->context, index, &argument)
				|| argument.ownership
					!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
				|| !zend_mir_w05_verify_scalar_result(
					view, argument.value_id, &representation)
				|| representation == ZEND_MIR_REPRESENTATION_VOID) {
			goto failure;
		}
	}
	for (index = 0; index < calls->call_site_count(calls->context); index++) {
		zend_mir_call_site_ref site;
		zend_mir_representation representation;
		if (!calls->call_site_at(calls->context, index, &site)) {
			goto failure;
		}
		if (zend_mir_id_is_valid(site.result_id)
				&& (!zend_mir_w05_verify_scalar_result(
						view, site.result_id, &representation)
					|| representation
						== ZEND_MIR_REPRESENTATION_VOID)) {
			goto failure;
		}
	}
	return true;

failure:
	zend_mir_w05_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_W05_ARGUMENT_MISMATCH,
		ZEND_MIRV_TOKEN_W05_ARGUMENT_MISMATCH);
	return false;
}

static bool zend_mir_w05_block_has_predecessor(
	const zend_mir_view *view, zend_mir_block_id block_id,
	zend_mir_block_id predecessor)
{
	uint32_t index;
	uint32_t count = view->predecessor_count(view->context, block_id);
	if (count > view->block_count(view->context)) {
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_block_id candidate;
		if (!view->predecessor_at(
				view->context, block_id, index, &candidate)) {
			return false;
		}
		if (candidate == predecessor) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_block_has_successor(
	const zend_mir_view *view, zend_mir_block_id block_id,
	zend_mir_block_id successor)
{
	uint32_t index;
	uint32_t count = view->successor_count(view->context, block_id);
	if (count > view->block_count(view->context)) {
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_block_id candidate;
		if (!view->successor_at(
				view->context, block_id, index, &candidate)) {
			return false;
		}
		if (candidate == successor) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w05_verify_final_control_flow(
	const zend_mir_view *view, const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (zend_mir_w05_test_fault_is(
			ZEND_MIR_W05_TEST_FAULT_CONTROL_FLOW_VERIFIER)
			|| view == NULL || calls == NULL
			|| view->block_count == NULL || view->block_at == NULL
			|| view->successor_count == NULL
			|| view->successor_at == NULL
			|| view->predecessor_count == NULL
			|| view->predecessor_at == NULL
			|| calls->call_site_count == NULL
			|| calls->call_site_at == NULL
			|| calls->call_continuation_at == NULL) {
		goto failure;
	}
	for (index = 0; index < view->block_count(view->context); index++) {
		zend_mir_block_record block;
		uint32_t edge_index;
		uint32_t successor_count;
		uint32_t predecessor_count;
		if (!view->block_at(view->context, index, &block)) {
			goto failure;
		}
		successor_count = view->successor_count(
			view->context, block.id);
		predecessor_count = view->predecessor_count(
			view->context, block.id);
		if (successor_count > view->block_count(view->context)
				|| predecessor_count
					> view->block_count(view->context)) {
			goto failure;
		}
		for (edge_index = 0;
			edge_index < successor_count; edge_index++) {
			zend_mir_block_id successor;
			if (!view->successor_at(
					view->context, block.id, edge_index, &successor)
					|| !zend_mir_w05_find_block(
						view, successor, NULL)
					|| !zend_mir_w05_block_has_predecessor(
						view, successor, block.id)) {
				goto failure;
			}
		}
		for (edge_index = 0;
			edge_index < predecessor_count; edge_index++) {
			zend_mir_block_id predecessor;
			if (!view->predecessor_at(
					view->context, block.id, edge_index,
					&predecessor)
					|| !zend_mir_w05_find_block(
						view, predecessor, NULL)
					|| !zend_mir_w05_block_has_successor(
						view, predecessor, block.id)) {
				goto failure;
			}
		}
	}
	for (index = 0; index < calls->call_site_count(calls->context); index++) {
		zend_mir_call_site_ref site;
		zend_mir_call_continuation_ref continuation;
		zend_mir_instruction_record instruction;
		if (!calls->call_site_at(calls->context, index, &site)
				|| !view->instruction_at(
					view->context, site.instruction_id, &instruction)
				|| instruction.opcode
					!= ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| zend_mir_opcode_is_terminator(instruction.opcode)
				|| !calls->call_continuation_at(
					calls->context, site.continuations.offset,
					&continuation)
				|| continuation.kind
					!= ZEND_MIR_CALL_CONTINUATION_NORMAL
				|| continuation.block_id != instruction.block_id) {
			goto failure;
		}
	}
	return true;

failure:
	zend_mir_w05_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_W05_CONTINUATION_MISMATCH,
		ZEND_MIRV_TOKEN_W05_CONTINUATION_MISMATCH);
	return false;
}

static bool zend_mir_w05_verify_final_composition(
	const zend_mir_view *view,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	if (!zend_mir_w05_verify_final_structural(view, diagnostics)) {
		return false;
	}
	if (!zend_mir_w05_verify_final_scalar(
			view, calls, diagnostics)) {
		return false;
	}
	if (!zend_mir_w05_verify_final_control_flow(
			view, calls, diagnostics)) {
		return false;
	}
	return !zend_mir_w05_test_fault_is(
			ZEND_MIR_W05_TEST_FAULT_CALL_VERIFIER)
		&& zend_mir_verify_w05_calls(
			view, source_calls, calls, diagnostics);
}

zend_mir_w05_lowering_result zend_mir_lower_w05_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator)
{
	zend_mir_w05_plan plan;
	zend_mir_lowering_result w04;
	zend_mir_lowering_diagnostic_code code;
	const zend_mir_view *view;
	const zend_mir_call_view *calls;
	uint32_t module_fingerprint[4];
	uint32_t source_fingerprint[4];
	uint32_t recomputed_module_fingerprint[4];
	uint32_t recomputed_source_fingerprint[4];
	zend_mir_w05_lowering_result result;

	code = zend_mir_w05_plan_calls(
		context, source_calls, resolver, &plan);
	if (code != ZEND_MIRL_OK) {
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			code == ZEND_MIRL_W05_CALL_PLAN_FAILED
				|| code == ZEND_MIRL_W05_MALFORMED_CALL_SEQUENCE
				? ZEND_MIR_LOWERING_FAILED : ZEND_MIR_LOWERING_DEFERRED,
			code);
	}
	w04 = zend_mir_lower_w04_zend_source(context, mutator, control_flow_map);
	if (w04.status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_w05_plan_release(&plan);
		result = zend_mir_w05_failure(w04.status, w04.diagnostic_code);
		return result;
	}
	if (!zend_mir_lowering_result_is_w04_failure_atomic(&w04)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W05_CALL_VERIFY_FAILED);
	}
	if (call_mutator == NULL) {
		call_mutator = zend_mir_module_get_call_mutator(w04.module);
	}
	if (!zend_mir_w05_emit_calls(&plan, context, call_mutator)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W05_CALL_PLAN_FAILED);
	}
	if (!context->module_ops.finalize(
			context->module_ops.context, w04.module)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W05_CALL_PLAN_FAILED);
	}
	view = context->module_ops.view(context->module_ops.context, w04.module);
	calls = zend_mir_module_get_call_view(w04.module);
	if (view == NULL || calls == NULL
			|| !zend_mir_w05_verify_final_composition(
				view, source_calls, calls, context->diagnostics)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W05_CALL_VERIFY_FAILED);
	}
	/* Recompute the stable projection to catch mutation during verification. */
	if (!zend_mir_w05_build_fingerprints(
				view, context->source, source_calls, calls,
				context->diagnostics,
				module_fingerprint, source_fingerprint)
			|| !zend_mir_w05_build_fingerprints(
				view, context->source, source_calls, calls,
				context->diagnostics,
				recomputed_module_fingerprint,
				recomputed_source_fingerprint)
#ifdef ZEND_MIR_W05_TEST_FAULTS
			|| zend_mir_w05_test_fault_is(
				ZEND_MIR_W05_TEST_FAULT_FINGERPRINT_RECOMPUTE)
#endif
			|| !zend_mir_w05_words_equal(
				module_fingerprint, recomputed_module_fingerprint)
			|| !zend_mir_w05_words_equal(
				source_fingerprint, recomputed_source_fingerprint)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		zend_mir_w05_plan_release(&plan);
		return zend_mir_w05_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W05_CALL_VERIFY_FAILED);
	}
	zend_mir_w05_plan_release(&plan);
	memset(&result, 0, sizeof(result));
	result.lowering = w04;
	result.prerequisite_guarantees = w04.guarantees;
	result.lowering.guarantees = ZEND_MIR_LOWERING_GUARANTEE_FINALIZED;
	result.capabilities = ZEND_MIR_W05_REQUIRED_CAPABILITIES;
	result.semantic_debts = ZEND_MIR_W05_REQUIRED_DEBTS;
	result.modeled = true;
	result.codegen_eligible = false;
	return result;
}
