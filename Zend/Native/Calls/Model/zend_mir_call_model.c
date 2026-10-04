#include "zend_mir_call_model.h"

#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "../../Lowering/Core/zend_mir_lowering_internal.h"
#include "../../Lowering/Frontend/zend_mir_zend_source.h"
#include "../../MIR/Core/zend_mir_module_internal.h"
#include "../../MIR/zend_mir_id_index.h"

typedef struct _zend_mir_plan {
	zend_mir_call_plan public_plan;
	zend_mir_call_plan_entry *entries;
	zend_mir_source_call_site_ref *sites;
	zend_mir_source_call_target_ref *targets;
	zend_mir_source_call_argument_ref *arguments;
	zend_mir_value_id *values;
	zend_mir_call_argument_ownership *ownerships;
	zend_mir_value_id *results;
	zend_mir_block_id *blocks;
	zend_mir_block_id *exception_blocks;
	zend_mir_block_id *source_block_ranks;
	uint32_t *exception_oplines;
	uint32_t site_count;
	uint32_t target_count;
	uint32_t argument_count;
	uint32_t source_block_count;
} zend_mir_plan;

#define EFFECTS \
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
#define READS \
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
#define WRITES READS
#define BARRIERS \
	(ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SAFEPOINT) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_REENTRANCY) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_BAILOUT) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER) \
	| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT))

typedef enum _zend_mir_allocation_kind {
	ZEND_MIR_ALLOCATION_PLANNER,
	ZEND_MIR_ALLOCATION_TARGET_SNAPSHOT,
	ZEND_MIR_ALLOCATION_ARGUMENT_TABLE
} zend_mir_allocation_kind;

static void *zend_mir_call_calloc(
	uint32_t count, size_t size, zend_mir_allocation_kind kind)
{
	if (size != 0 && (size_t) count > SIZE_MAX / size) {
		return NULL;
	}
	(void) kind;
	return calloc(count, size);
}

static bool zend_mir_target_equals(
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

static bool zend_mir_target_parameter_mode_at(
	const zend_mir_source_call_view *calls,
	const zend_mir_source_call_target_ref *target,
	uint32_t ordinal, zend_mir_source_parameter_mode *mode_out)
{
	zend_mir_source_parameter_mode_ref mode;
	uint32_t count;
	uint32_t parameter_ordinal = ordinal;
	uint32_t expected_mode_count;

	if (calls == NULL || target == NULL || mode_out == NULL
			|| calls->parameter_mode_count == NULL
			|| calls->parameter_mode_at == NULL) {
		return false;
	}
	if (target->variadic && target->num_args == UINT32_MAX) {
		return false;
	}
	expected_mode_count =
		target->num_args + (target->variadic ? 1 : 0);
	count = calls->parameter_mode_count(calls->context);
	if (target->parameter_modes.offset > count
			|| target->parameter_modes.count
				> count - target->parameter_modes.offset
			|| target->parameter_modes.count != expected_mode_count
			|| target->parameter_modes.count == 0) {
		return false;
	}
	if (parameter_ordinal >= target->num_args) {
		if (!target->variadic) {
			return false;
		}
		parameter_ordinal = target->num_args;
	}
	if (!calls->parameter_mode_at(calls->context,
			target->parameter_modes.offset + parameter_ordinal, &mode)
			|| mode.target_id != target->id
			|| mode.ordinal != parameter_ordinal
			|| (mode.mode != ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
				&& mode.mode != ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE)) {
		return false;
	}
	*mode_out = mode.mode;
	return true;
}

static zend_mir_lowering_result zend_mir_failure(
	zend_mir_lowering_status status, zend_mir_lowering_diagnostic_code code)
{
	zend_mir_lowering_result result;

	memset(&result, 0, sizeof(result));
	result.status = status;
	result.diagnostic_code = code;
	return result;
}

static void zend_mir_plan_release(zend_mir_plan *plan)
{
	if (plan == NULL) {
		return;
	}
	free(plan->entries);
	free(plan->sites);
	free(plan->targets);
	free(plan->arguments);
	free(plan->values);
	free(plan->ownerships);
	free(plan->results);
	free(plan->blocks);
	free(plan->exception_blocks);
	free(plan->source_block_ranks);
	free(plan->exception_oplines);
	memset(plan, 0, sizeof(*plan));
}

static bool zend_mir_build_source_block_ranks(
	const zend_mir_lowering_source_view *source, zend_mir_plan *plan)
{
	uint8_t *seen = NULL;
	uint32_t index;
	uint32_t rank = 0;

	if (source == NULL || source->block_count == NULL
			|| source->block_at == NULL || plan == NULL) {
		return false;
	}
	plan->source_block_count = source->block_count(source->context);
	if (plan->source_block_count == 0) {
		return false;
	}
	plan->source_block_ranks = zend_mir_call_calloc(
		plan->source_block_count, sizeof(*plan->source_block_ranks),
		ZEND_MIR_ALLOCATION_PLANNER);
	seen = zend_mir_call_calloc(
		plan->source_block_count, sizeof(*seen),
		ZEND_MIR_ALLOCATION_PLANNER);
	if (plan->source_block_ranks == NULL || seen == NULL) {
		free(seen);
		return false;
	}
	for (index = 0; index < plan->source_block_count; index++) {
		zend_mir_source_block_ref block;
		if (!source->block_at(source->context, index, &block)
				|| block.id >= plan->source_block_count || seen[block.id]) {
			free(seen);
			return false;
		}
		seen[block.id] = 1;
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0) {
			plan->source_block_ranks[block.id] = rank++;
		} else {
			plan->source_block_ranks[block.id] = ZEND_MIR_ID_INVALID;
		}
	}
	free(seen);
	return true;
}

static bool zend_mir_source_block_to_mir(
	const zend_mir_plan *plan, zend_mir_source_block_id source_id,
	zend_mir_block_id *out)
{
	if (plan == NULL || out == NULL || source_id >= plan->source_block_count
			|| plan->source_block_ranks == NULL
			|| !zend_mir_id_is_valid(plan->source_block_ranks[source_id])) {
		return false;
	}
	*out = plan->source_block_ranks[source_id];
	return true;
}

static bool zend_mir_result_value(
	const zend_mir_lowering_context *context,
	const zend_mir_source_call_site_ref *site,
	bool allow_boxed,
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
	if (!zend_mir_id_is_valid(site->result_ssa_variable_id)) {
		return false;
	}
	value = zend_mir_value_from_original_ssa(
		site->result_ssa_variable_id);
	if (result_flags == 0 && allow_boxed) {
		*out = value;
		return true;
	}
	if (result_flags != ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR) {
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

static bool zend_mir_prior_call_result(
	const zend_mir_plan *plan, uint32_t site_index,
	zend_mir_value_id value)
{
	uint32_t index;

	for (index = 0; index < site_index; index++) {
		if (plan->results[index] == value) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_is_call_init(uint32_t opcode)
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

static bool zend_mir_is_call_send(uint32_t opcode)
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

static bool zend_mir_is_supported_send(
	uint32_t opcode)
{
	return opcode == ZEND_SEND_VAL || opcode == ZEND_SEND_VAL_EX
		|| opcode == ZEND_SEND_VAR || opcode == ZEND_SEND_VAR_EX
		|| (opcode == ZEND_SEND_REF)
		|| ((opcode == ZEND_SEND_VAR_NO_REF
			|| opcode == ZEND_SEND_VAR_NO_REF_EX
			|| opcode == ZEND_SEND_UNPACK
			|| opcode == ZEND_SEND_ARRAY
			|| opcode == ZEND_SEND_USER))
		|| ((opcode == ZEND_SEND_FUNC_ARG
			|| opcode == ZEND_SEND_PLACEHOLDER));
}

static bool zend_mir_is_call_finish(uint32_t opcode)
{
	return opcode == ZEND_DO_UCALL || opcode == ZEND_DO_FCALL
		|| opcode == ZEND_DO_FCALL_BY_NAME || opcode == ZEND_DO_ICALL
		|| opcode == ZEND_CALLABLE_CONVERT
		|| opcode == ZEND_CALLABLE_CONVERT_PARTIAL;
}

static zend_mir_lowering_diagnostic_code zend_mir_source_sequence(
	const zend_mir_source_call_view *calls)
{
	zend_mir_source_call_site_id *stack = NULL;
	zend_mir_source_call_site_id *init_sites = NULL;
	zend_mir_source_call_site_id *finish_sites = NULL;
	zend_mir_source_call_argument_id *arguments = NULL;
	bool *seen_arguments = NULL;
	uint32_t source_count;
	uint32_t site_count;
	uint32_t argument_count;
	uint32_t stack_count = 0;
	uint32_t index;
	zend_mir_lowering_diagnostic_code result =
		ZEND_MIRL_MALFORMED_CALL_SEQUENCE;

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
	stack = zend_mir_call_calloc(
		site_count, sizeof(*stack), ZEND_MIR_ALLOCATION_PLANNER);
	seen_arguments = zend_mir_call_calloc(
		argument_count == 0 ? 1 : argument_count,
		sizeof(*seen_arguments), ZEND_MIR_ALLOCATION_PLANNER);
	init_sites = zend_mir_call_calloc(
		source_count, sizeof(*init_sites), ZEND_MIR_ALLOCATION_PLANNER);
	finish_sites = zend_mir_call_calloc(
		source_count, sizeof(*finish_sites), ZEND_MIR_ALLOCATION_PLANNER);
	arguments = zend_mir_call_calloc(
		source_count, sizeof(*arguments), ZEND_MIR_ALLOCATION_PLANNER);
	if (stack == NULL || seen_arguments == NULL || init_sites == NULL
			|| finish_sites == NULL || arguments == NULL) {
		result = ZEND_MIRL_CALL_PLAN_FAILED;
		goto done;
	}
	for (index = 0; index < source_count; index++) {
		init_sites[index] = ZEND_MIR_ID_INVALID;
		finish_sites[index] = ZEND_MIR_ID_INVALID;
		arguments[index] = ZEND_MIR_ID_INVALID;
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
								>= site.init_opline_index))
				|| zend_mir_id_is_valid(init_sites[site.init_opline_index])
				|| zend_mir_id_is_valid(finish_sites[site.do_opline_index])) {
			goto done;
		}
		init_sites[site.init_opline_index] = site.id;
		finish_sites[site.do_opline_index] = site.id;
	}
	for (index = 0; index < argument_count; index++) {
		zend_mir_source_call_argument_ref argument;
		if (!calls->call_argument_at(calls->context, index, &argument)
				|| argument.id != index
				|| argument.call_site_id >= site_count
				|| argument.send_opline_index >= source_count
				|| zend_mir_id_is_valid(arguments[argument.send_opline_index])) {
			goto done;
		}
		arguments[argument.send_opline_index] = argument.id;
	}
	for (index = 0; index < source_count; index++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_source_call_site_id init_id = init_sites[index];
		zend_mir_source_call_site_id finish_id = finish_sites[index];
		zend_mir_source_call_argument_id argument_id = arguments[index];

		if (!calls->source_opcode_at(calls->context, index, &opcode)) {
			goto done;
		}
		if (zend_mir_id_is_valid(init_id)) {
			zend_mir_source_call_site_ref site_record;
			zend_mir_source_call_site_ref *site = &site_record;
			zend_mir_source_call_target_ref target;
			zend_mir_source_call_site_id parent =
				stack_count == 0
					? ZEND_MIR_ID_INVALID : stack[stack_count - 1];
			bool nested = stack_count != 0;
			if (!zend_mir_is_call_init(opcode.zend_opcode_number)
					|| !calls->call_site_at(
						calls->context, init_id, site)
					|| calls->call_target_at == NULL
					|| !calls->call_target_at(
						calls->context, site->target_id, &target)
					|| (target.kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
						? ((opcode.zend_opcode_number != ZEND_INIT_FCALL
								&& opcode.zend_opcode_number
									!= ZEND_INIT_FCALL_BY_NAME
								&& opcode.zend_opcode_number
									!= ZEND_INIT_NS_FCALL_BY_NAME))
						: target.kind
							== ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER
							? ((opcode.zend_opcode_number
									!= ZEND_INIT_FCALL
									&& opcode.zend_opcode_number
										!= ZEND_INIT_FCALL_BY_NAME
									&& opcode.zend_opcode_number
										!= ZEND_INIT_NS_FCALL_BY_NAME
									&& opcode.zend_opcode_number
										!= ZEND_INIT_DYNAMIC_CALL
									&& opcode.zend_opcode_number
										!= ZEND_INIT_USER_CALL))
						: target.kind == ZEND_MIR_SOURCE_CALL_TARGET_METHOD
							? ((opcode.zend_opcode_number
									!= ZEND_INIT_METHOD_CALL
									&& opcode.zend_opcode_number
										!= ZEND_INIT_STATIC_METHOD_CALL
									&& opcode.zend_opcode_number
										!= ZEND_INIT_PARENT_PROPERTY_HOOK_CALL
									&& opcode.zend_opcode_number != ZEND_NEW))
						: (target.kind
								!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
							|| (opcode.zend_opcode_number
									!= ZEND_INIT_FCALL
								&& opcode.zend_opcode_number
									!= ZEND_INIT_METHOD_CALL
								&& opcode.zend_opcode_number
									!= ZEND_INIT_STATIC_METHOD_CALL
								&& (opcode.zend_opcode_number
										!= ZEND_NEW))))
					|| site->parent_call_site_id != parent
					|| ((site->flags
							& ZEND_MIR_SOURCE_CALL_SITE_NESTED) != 0)
						!= nested
					|| stack_count >= site_count) {
				result = zend_mir_is_call_init(
					opcode.zend_opcode_number)
					? ZEND_MIRL_UNSUPPORTED_TARGET
					: ZEND_MIRL_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			stack[stack_count++] = site->id;
		} else if (zend_mir_is_call_init(opcode.zend_opcode_number)) {
			/* The frontend inventory intentionally excludes unreachable blocks.
			 * Their source opcodes remain addressable for diagnostics, but they
			 * cannot participate in the executable call sequence. */
			continue;
		}
		if (zend_mir_id_is_valid(argument_id)) {
			zend_mir_source_call_argument_ref argument_record;
			zend_mir_source_call_argument_ref *argument = &argument_record;
			if (!zend_mir_is_call_send(opcode.zend_opcode_number)
					|| !zend_mir_is_supported_send(
						opcode.zend_opcode_number)
					|| !calls->call_argument_at(
						calls->context, argument_id, argument)
					|| seen_arguments[argument->id]
					|| argument->flags != 0
					|| stack_count == 0
					|| stack[stack_count - 1] != argument->call_site_id) {
				result = zend_mir_is_call_send(
					opcode.zend_opcode_number)
					? ZEND_MIRL_UNSUPPORTED_ARGUMENT
					: ZEND_MIRL_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			seen_arguments[argument->id] = true;
		} else if (zend_mir_is_call_send(opcode.zend_opcode_number)) {
			continue;
		}
		if (zend_mir_id_is_valid(finish_id)) {
			zend_mir_source_call_site_ref site;
			zend_mir_source_call_target_ref target;
			if (!zend_mir_is_call_finish(opcode.zend_opcode_number)
					|| !calls->call_site_at(
						calls->context, finish_id, &site)
					|| calls->call_target_at == NULL
					|| !calls->call_target_at(
						calls->context, site.target_id, &target)
					|| (target.kind
						== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
						? ((opcode.zend_opcode_number != ZEND_DO_ICALL
								&& opcode.zend_opcode_number != ZEND_DO_FCALL
								&& opcode.zend_opcode_number
									!= ZEND_DO_FCALL_BY_NAME
								&& ((opcode.zend_opcode_number
											!= ZEND_CALLABLE_CONVERT
										&& opcode.zend_opcode_number
											!= ZEND_CALLABLE_CONVERT_PARTIAL))))
					: ((target.kind
								!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
							&& (target.kind
								!= ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER)
							&& (target.kind
								!= ZEND_MIR_SOURCE_CALL_TARGET_METHOD))
							|| (target.kind
									== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
								&& (opcode.zend_opcode_number
										!= ZEND_DO_UCALL
									&& opcode.zend_opcode_number
										!= ZEND_DO_FCALL
									&& opcode.zend_opcode_number
										!= ZEND_DO_FCALL_BY_NAME
									&& opcode.zend_opcode_number
										!= ZEND_DO_ICALL
									&& opcode.zend_opcode_number
										!= ZEND_CALLABLE_CONVERT
									&& (opcode.zend_opcode_number
											!= ZEND_CALLABLE_CONVERT_PARTIAL)))))
					|| stack_count == 0
					|| stack[stack_count - 1] != finish_id) {
				result = zend_mir_is_call_finish(
					opcode.zend_opcode_number)
					? ZEND_MIRL_UNSUPPORTED_TARGET
					: ZEND_MIRL_MALFORMED_CALL_SEQUENCE;
				goto done;
			}
			stack_count--;
		} else if (zend_mir_is_call_finish(opcode.zend_opcode_number)) {
			continue;
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
	free(arguments);
	free(finish_sites);
	free(init_sites);
	free(seen_arguments);
	free(stack);
	return result;
}

static zend_mir_lowering_diagnostic_code zend_mir_plan_calls(
	zend_mir_lowering_context *context,
	const zend_mir_source_call_view *calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_plan *plan,
	bool allow_empty_calls)
{
	uint32_t index;

	if (context == NULL || calls == NULL || resolver == NULL || plan == NULL
			|| calls->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| calls->call_site_count == NULL || calls->call_site_at == NULL
			|| calls->call_target_count == NULL || calls->call_target_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->source_opcode_count == NULL
			|| calls->source_opcode_at == NULL
			|| resolver->resolve_exact_direct_user == NULL
			|| (resolver->resolve_exact_internal == NULL)) {
		return ZEND_MIRL_CALL_PLAN_FAILED;
	}
	memset(plan, 0, sizeof(*plan));
	plan->site_count = calls->call_site_count(calls->context);
	plan->target_count = calls->call_target_count(calls->context);
	plan->argument_count = calls->call_argument_count(calls->context);
	if (plan->site_count == 0 && plan->target_count == 0
			&& plan->argument_count == 0 && allow_empty_calls) {
		return ZEND_MIRL_OK;
	}
	if (plan->site_count == 0 || plan->target_count == 0) {
		return ZEND_MIRL_RUNTIME_EFFECT_DEFERRED;
	}
	if (plan->site_count > ZEND_MIR_ID_MAX / 4) {
		return ZEND_MIRL_CALL_PLAN_FAILED;
	}
	plan->entries = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->entries),
		ZEND_MIR_ALLOCATION_PLANNER);
	plan->sites = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->sites),
		ZEND_MIR_ALLOCATION_PLANNER);
	plan->targets = zend_mir_call_calloc(
		plan->target_count, sizeof(*plan->targets),
		ZEND_MIR_ALLOCATION_TARGET_SNAPSHOT);
	plan->arguments = zend_mir_call_calloc(
		plan->argument_count, sizeof(*plan->arguments),
		ZEND_MIR_ALLOCATION_ARGUMENT_TABLE);
	plan->values = zend_mir_call_calloc(
		plan->argument_count, sizeof(*plan->values),
		ZEND_MIR_ALLOCATION_ARGUMENT_TABLE);
	plan->ownerships = zend_mir_call_calloc(
		plan->argument_count, sizeof(*plan->ownerships),
		ZEND_MIR_ALLOCATION_ARGUMENT_TABLE);
	plan->results = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->results),
		ZEND_MIR_ALLOCATION_PLANNER);
	plan->blocks = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->blocks),
		ZEND_MIR_ALLOCATION_PLANNER);
	plan->exception_blocks = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->exception_blocks),
		ZEND_MIR_ALLOCATION_PLANNER);
	plan->exception_oplines = zend_mir_call_calloc(
		plan->site_count, sizeof(*plan->exception_oplines),
		ZEND_MIR_ALLOCATION_PLANNER);
	if (plan->entries == NULL || plan->sites == NULL || plan->targets == NULL
			|| (plan->argument_count != 0
				&& (plan->arguments == NULL || plan->values == NULL
					|| plan->ownerships == NULL))
			|| plan->results == NULL || plan->blocks == NULL
			|| plan->exception_blocks == NULL
			|| plan->exception_oplines == NULL) {
		return ZEND_MIRL_CALL_PLAN_FAILED;
	}
	if (!zend_mir_build_source_block_ranks(context->source, plan)) {
		return ZEND_MIRL_CALL_PLAN_FAILED;
	}
	for (index = 0; index < plan->site_count; index++) {
		plan->exception_blocks[index] = ZEND_MIR_ID_INVALID;
		plan->exception_oplines[index] = ZEND_MIR_ID_INVALID;
	}
	for (index = 0; index < plan->argument_count; index++) {
		plan->values[index] = ZEND_MIR_ID_INVALID;
		plan->ownerships[index] = ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
		if (!calls->call_argument_at(calls->context, index,
				&plan->arguments[index])) {
			return ZEND_MIRL_UNSUPPORTED_ARGUMENT;
		}
		if (plan->arguments[index].id != index
				|| ((plan->arguments[index].mode
							!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
						&& plan->arguments[index].mode
							!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
						&& plan->arguments[index].mode
							!= ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED
						&& plan->arguments[index].mode
							!= ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK
						&& (plan->arguments[index].mode
								!= ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER)))
				|| plan->arguments[index].flags != 0
				|| zend_mir_id_is_valid(
					plan->arguments[index].name_symbol_id)) {
			return ZEND_MIRL_UNSUPPORTED_ARGUMENT;
		}
	}
	for (index = 0; index < plan->target_count; index++) {
		zend_mir_source_call_target_ref resolved;
		if (!calls->call_target_at(calls->context, index,
				&plan->targets[index])
				|| plan->targets[index].id != index) {
			return ZEND_MIRL_UNSUPPORTED_TARGET;
		}
		if (plan->targets[index].kind
				== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL) {
			if (!resolver->resolve_exact_internal(
						resolver->context, index, &resolved)
				|| !zend_mir_target_equals(
						&resolved, &plan->targets[index])
					|| resolved.kind
						!= ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
					|| (resolved.parameter_modes.count
							!= resolved.num_args
						&& (!resolved.variadic
						|| resolved.num_args == UINT32_MAX
						|| resolved.parameter_modes.count
							!= resolved.num_args + 1))) {
				return ZEND_MIRL_UNSUPPORTED_TARGET;
			}
		} else if (plan->targets[index].kind
				== ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER) {
			if (!plan->targets[index].variadic
					|| plan->targets[index].required_num_args != 0
					|| plan->targets[index].parameter_modes.count != 0) {
				return ZEND_MIRL_UNSUPPORTED_TARGET;
			}
		} else if (!(plan->targets[index].kind
				== ZEND_MIR_SOURCE_CALL_TARGET_METHOD
				? resolver->resolve_exact_method != NULL
					&& resolver->resolve_exact_method(
						resolver->context, index, &resolved)
				: resolver->resolve_exact_direct_user(
					resolver->context, index, &resolved))
				|| !zend_mir_target_equals(
					&resolved, &plan->targets[index])
				|| (resolved.kind != ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
					&& (resolved.kind
						!= ZEND_MIR_SOURCE_CALL_TARGET_METHOD))) {
			return ZEND_MIRL_UNSUPPORTED_TARGET;
		}
	}
	for (index = 0; index < plan->site_count; index++) {
		zend_mir_source_call_site_ref *site = &plan->sites[index];
		zend_mir_call_plan_entry *entry = &plan->entries[index];
		zend_mir_source_opcode_ref do_opcode;
		bool site_borrows_scalars;
		uint32_t argument_index;
		if (!calls->call_site_at(calls->context, index, site)
				|| site->id != index || site->target_id >= plan->target_count
				|| site->argument_span.offset > plan->argument_count
				|| site->argument_span.count
					> plan->argument_count - site->argument_span.offset
				|| site->do_opline_index
					>= calls->source_opcode_count(calls->context)
				|| !calls->source_opcode_at(
					calls->context, site->do_opline_index, &do_opcode)
				|| do_opcode.opline_index != site->do_opline_index
				|| memcmp(&site->result_operand, &do_opcode.result,
					sizeof(site->result_operand)) != 0) {
			return ZEND_MIRL_MALFORMED_CALL_SEQUENCE;
		}
		/*
		 * A call instruction either carries every argument as a machine
		 * operand or carries none of them and lets the source-backed runtime
		 * consume the canonical zval slots.  Do not create a mixed ownership
		 * model: there is no operand-to-argument map for such an instruction.
		 */
		site_borrows_scalars = plan->targets[site->target_id].kind
				== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER;
		for (argument_index = 0;
			site_borrows_scalars
				&& argument_index < site->argument_span.count;
			argument_index++) {
			const uint32_t plan_argument_index =
				site->argument_span.offset + argument_index;
			const zend_mir_source_call_argument_ref *argument =
				&plan->arguments[plan_argument_index];
			const zend_mir_source_call_target_ref *target =
				&plan->targets[site->target_id];
			zend_mir_source_parameter_mode parameter_mode;
			bool value_source_mode = argument->mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
				|| argument->mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED;

			if (argument_index >= target->num_args && !target->variadic) {
				parameter_mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
			} else if (!zend_mir_target_parameter_mode_at(
					calls, target, argument_index, &parameter_mode)) {
				return ZEND_MIRL_UNSUPPORTED_ARGUMENT;
			}
			site_borrows_scalars =
				parameter_mode == ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
				&& value_source_mode
				&& zend_mir_id_is_valid(plan->values[plan_argument_index]);
		}
		if (plan->targets[site->target_id].kind
				== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL) {
			uint32_t result_flags = site->flags
				& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
					| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);

			if (result_flags == ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED) {
				if (zend_mir_id_is_valid(site->result_ssa_variable_id)) {
					return ZEND_MIRL_UNSUPPORTED_RESULT;
				}
				plan->results[index] = ZEND_MIR_ID_INVALID;
			} else if (result_flags == 0
					&& !zend_mir_id_is_valid(
						site->result_ssa_variable_id)) {
				/* Refcounted and otherwise non-scalar internal results remain
				 * in their source zval slot. A later source-backed opcode
				 * must independently prove and consume that exact slot. */
				plan->results[index] = ZEND_MIR_ID_INVALID;
			} else if (result_flags
					== ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
					&& zend_mir_id_is_valid(
						site->result_ssa_variable_id)) {
				/*
				 * Exact non-refcounted internal results enter scalar MIR.
				 * Every other zval result stays in its canonical source slot
				 * and can be consumed by another source-backed operation.
				 */
				if (!zend_mir_result_value(
						context, site, false, &plan->results[index])) {
					plan->results[index] = ZEND_MIR_ID_INVALID;
				}
			} else {
				return ZEND_MIRL_UNSUPPORTED_RESULT;
			}
		} else {
			/*
			 * Arbitrary user-call results stay in the canonical source zval
			 * slot. The scalar overlay is different: an exact declared result
			 * consumed by scalar SSA is the machine result of the call
			 * instruction itself.
			 */
			if (!zend_mir_result_value(
						context, site, true, &plan->results[index])) {
				plan->results[index] = ZEND_MIR_ID_INVALID;
			}
		}
		for (argument_index = 0;
				argument_index < site->argument_span.count;
				argument_index++) {
			const zend_mir_source_call_argument_ref *argument =
				&plan->arguments[site->argument_span.offset + argument_index];
			if (argument->call_site_id != site->id
					|| argument->ordinal != argument_index) {
				return ZEND_MIRL_MALFORMED_CALL_SEQUENCE;
			}
			if (plan->targets[site->target_id].kind
					== ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL) {
				zend_mir_source_parameter_mode parameter_mode;

				/* A partial-application placeholder describes a future Closure
				 * parameter; it is not an argument passed to the selected internal
				 * function while the partial is constructed.  In particular, PHP
				 * must reach the runtime's normal dynamic-call and argument-count
				 * validation even when the placeholder has no corresponding declared
				 * parameter. */
				if (argument->mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
						|| argument->mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK) {
					plan->ownerships[
						site->argument_span.offset + argument_index] =
							ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE;
					plan->values[
						site->argument_span.offset + argument_index] =
							ZEND_MIR_ID_INVALID;
					continue;
				}
				/* A named argument's syntactic ordinal is not its declared
				 * parameter ordinal.  The call model has no parameter name in
				 * this view, so defer the actual internal by-reference decision
				 * to descriptor construction, where the zend_function and SEND
				 * name are both available. */
				if (argument->mode == ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
					parameter_mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
				} else if (argument_index
						>= plan->targets[site->target_id].num_args
						&& !plan->targets[site->target_id].variadic) {
					/* Extra internal arguments are evaluated before the runtime
					 * reports the argument-count error. They have no declared
					 * parameter mode and therefore use ordinary by-value ownership. */
					parameter_mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
				} else if (!zend_mir_target_parameter_mode_at(
						calls, &plan->targets[site->target_id],
						argument_index, &parameter_mode)) {
					return ZEND_MIRL_UNSUPPORTED_ARGUMENT;
				}
				plan->ownerships[
					site->argument_span.offset + argument_index] =
					parameter_mode == ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
						? ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
						: ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE;
				plan->values[
					site->argument_span.offset + argument_index] =
					ZEND_MIR_ID_INVALID;
			} else {
				uint32_t plan_argument_index =
					site->argument_span.offset + argument_index;
				zend_mir_source_parameter_mode parameter_mode =
					argument->mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
						? ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
						: ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
				bool value_source_mode = argument->mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					|| argument->mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED;
				bool borrowed_scalar;

				if (plan->targets[site->target_id].kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER) {
					const zend_mir_source_call_target_ref *target =
						&plan->targets[site->target_id];

					if (argument_index >= target->num_args
							&& !target->variadic) {
						/*
						 * User functions retain positional extra arguments
						 * for func_get_args(), but those arguments have no
						 * declared parameter whose by-reference mode must be
						 * consulted.
						 */
						parameter_mode =
							ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
					} else if (!zend_mir_target_parameter_mode_at(
								calls, target, argument_index,
								&parameter_mode)) {
						return ZEND_MIRL_UNSUPPORTED_ARGUMENT;
					}
				}
				borrowed_scalar =
					site_borrows_scalars
					&& plan->targets[site->target_id].kind
						== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
					&& parameter_mode
						== ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
					&& value_source_mode
					&& zend_mir_id_is_valid(
						plan->values[plan_argument_index]);
				plan->ownerships[plan_argument_index] = borrowed_scalar
					? ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
					: parameter_mode
							== ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
						? ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
						: ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE;
				if (!borrowed_scalar) {
					zend_mir_value_id value =
						zend_mir_id_is_valid(
							argument->value_ssa_variable_id)
						? zend_mir_value_from_original_ssa(
							argument->value_ssa_variable_id)
						: ZEND_MIR_ID_INVALID;
					/*
					 * A nested direct call is the first arbitrary boxed
					 * producer whose machine definition is guaranteed here.
					 * Other source zvals remain frame-backed until their own
					 * lowering operation proves a register definition.
					 */
					plan->values[plan_argument_index] =
						parameter_mode
								== ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
							&& value_source_mode
							&& zend_mir_id_is_valid(value)
							&& zend_mir_prior_call_result(
								plan, index, value)
						? value : ZEND_MIR_ID_INVALID;
				}
			}
		}
		if (!zend_mir_source_block_to_mir(
				plan, site->source_block_id,
				&plan->blocks[index])) {
			return ZEND_MIRL_CALL_PLAN_FAILED;
		}
		if ((site->flags & ZEND_MIR_SOURCE_CALL_SITE_PROTECTED) != 0) {
			zend_mir_source_block_id handler_source_block;
			if (!zend_mir_zend_source_exception_handler(
					context->zend_source, site->do_opline_index,
					&handler_source_block, &plan->exception_oplines[index])
					|| !zend_mir_source_block_to_mir(
						plan, handler_source_block,
						&plan->exception_blocks[index])) {
				return ZEND_MIRL_CALL_PLAN_FAILED;
			}
		}
		entry->source_call_site_id = site->id;
		entry->decision = ZEND_MIR_CALL_PLAN_ACCEPTED;
		entry->diagnostic_code = ZEND_MIRL_OK;
		entry->argument_span = site->argument_span;
	}
	{
		zend_mir_lowering_diagnostic_code grammar =
			zend_mir_source_sequence(
				calls);
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

static bool zend_mir_emit_calls(
	const zend_mir_plan *plan,
	const zend_mir_lowering_context *context,
	zend_mir_call_mutator *mutator,
	bool allow_empty_calls)
{
	uint32_t index;

	if (plan == NULL || context == NULL || context->zend_source == NULL
			|| !zend_mir_id_is_valid(context->function_id)
			|| !zend_mir_id_is_valid(context->zend_source->op_array_id)
			|| mutator == NULL
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| mutator->add_call_target == NULL
			|| mutator->add_call_argument == NULL
			|| mutator->add_call_continuation == NULL
			|| mutator->add_call_site == NULL
			|| mutator->commit_call_model == NULL) {
		return false;
	}
	if (plan->site_count == 0 && plan->target_count == 0
			&& plan->argument_count == 0) {
		zend_mir_module *module = mutator->context;
		return allow_empty_calls && module != NULL
			&& &module->call_mutator == mutator
			&& zend_mir_module_commit_empty_call_model(module);
	}
	for (index = 0; index < plan->target_count; index++) {
		const zend_mir_source_call_target_ref *source = &plan->targets[index];
		zend_mir_call_target_ref target;
		memset(&target, 0, sizeof(target));
		target.id = source->id;
		target.kind = source->kind == ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
			? ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
			: source->kind == ZEND_MIR_SOURCE_CALL_TARGET_METHOD
				? ZEND_MIR_CALL_TARGET_METHOD_USER
				: source->kind == ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER
					? ZEND_MIR_CALL_TARGET_DYNAMIC
					: ZEND_MIR_CALL_TARGET_DIRECT_USER;
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
		argument.ownership = plan->ownerships[index];
		argument.send_opline_index =
			plan->arguments[index].send_opline_index;
		argument.source_mode = plan->arguments[index].mode;
		argument.source_operand = plan->arguments[index].source_operand;
		if (!mutator->add_call_argument(mutator->context, &argument)) {
			return false;
		}
	}
	for (index = 0; index < plan->site_count; index++) {
		const zend_mir_source_call_site_ref *source = &plan->sites[index];
		zend_mir_call_site_ref site;
		uint32_t continuation_index;
		memset(&site, 0, sizeof(site));
		for (continuation_index = 0; continuation_index < 4;
				continuation_index++) {
			zend_mir_call_continuation_ref continuation;
			memset(&continuation, 0, sizeof(continuation));
			continuation.id = index * 4 + continuation_index;
			continuation.call_site_id = index;
			continuation.kind =
				(zend_mir_call_continuation_kind) continuation_index;
			continuation.block_id = continuation_index == 0
				? plan->blocks[index]
				: continuation_index == 1
					? plan->exception_blocks[index] : ZEND_MIR_ID_INVALID;
			continuation.source_opline_index = continuation_index == 1
				? plan->exception_oplines[index] : ZEND_MIR_ID_INVALID;
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
		site.result_operand = source->result_operand;
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
		site.effects = EFFECTS;
		site.reads = READS;
		site.writes = WRITES;
		site.barriers = BARRIERS;
		site.source_init_opline_index = source->init_opline_index;
		site.source_do_opline_index = source->do_opline_index;
		if (!mutator->add_call_site(mutator->context, &site)) {
			return false;
		}
	}
	return mutator->commit_call_model(mutator->context);
}

static bool zend_mir_call_verify_emit(zend_mir_diagnostic_sink *diagnostics,
	zend_mir_verify_call_code code, const char *token)
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

static bool zend_mir_span_equal(zend_mir_span left, zend_mir_span right)
{
	return left.offset == right.offset && left.count == right.count;
}

#if !defined(NDEBUG)
typedef struct _zend_mir_fingerprint_digest {
	uint32_t words[4];
} zend_mir_fingerprint_digest;

static zend_mir_fingerprint_digest zend_mir_fingerprint_init(void)
{
	zend_mir_fingerprint_digest digest = {{
		UINT32_C(2166136261),
		UINT32_C(3339451269),
		UINT32_C(2593831049),
		UINT32_C(1268118805)
	}};
	return digest;
}

static zend_mir_fingerprint_digest zend_mir_fingerprint_mix(
	zend_mir_fingerprint_digest digest, uint32_t value)
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

static zend_mir_fingerprint_digest zend_mir_fingerprint_mix_u64(
	zend_mir_fingerprint_digest digest, uint64_t value)
{
	digest = zend_mir_fingerprint_mix(digest, (uint32_t) value);
	return zend_mir_fingerprint_mix(
		digest, (uint32_t) (value >> 32));
}

typedef struct _zend_mir_call_fingerprint_writer {
	zend_mir_fingerprint_digest digest;
} zend_mir_call_fingerprint_writer;

static bool zend_mir_fingerprint_write(
	void *context, const char *bytes, size_t length)
{
	zend_mir_call_fingerprint_writer *writer = context;
	size_t index;

	if (writer == NULL || (bytes == NULL && length != 0)) {
		return false;
	}
	/* Keep the four independent FNV lanes together so debug and sanitizer
	 * builds do not expand every emitted byte into sixteen scalar operations.
	 * The per-lane byte sequence, and therefore the fingerprint, is unchanged. */
#if defined(__GNUC__)
	typedef uint32_t zend_mir_fingerprint_vector
		__attribute__((vector_size(4 * sizeof(uint32_t))));
	zend_mir_fingerprint_vector digest = {
		writer->digest.words[0], writer->digest.words[1],
		writer->digest.words[2], writer->digest.words[3]
	};
	static const zend_mir_fingerprint_vector prime = {
		UINT32_C(16777619), UINT32_C(16777619),
		UINT32_C(16777619), UINT32_C(16777619)
	};
	static const zend_mir_fingerprint_vector domain_bytes[4] = {
		{UINT32_C(0x88), UINT32_C(0xd3),
		 UINT32_C(0x2e), UINT32_C(0x44)},
		{UINT32_C(0x6a), UINT32_C(0x08),
		 UINT32_C(0x8a), UINT32_C(0x73)},
		{UINT32_C(0x3f), UINT32_C(0xa3),
		 UINT32_C(0x19), UINT32_C(0x70)},
		{UINT32_C(0x24), UINT32_C(0x85),
		 UINT32_C(0x13), UINT32_C(0x03)}
	};
	for (index = 0; index < length; index++) {
		const uint32_t value = (unsigned char) bytes[index];
		const zend_mir_fingerprint_vector input = {
			value, value, value, value
		};

		digest = (digest ^ input ^ domain_bytes[0]) * prime;
		digest = (digest ^ domain_bytes[1]) * prime;
		digest = (digest ^ domain_bytes[2]) * prime;
		digest = (digest ^ domain_bytes[3]) * prime;
	}
	writer->digest.words[0] = digest[0];
	writer->digest.words[1] = digest[1];
	writer->digest.words[2] = digest[2];
	writer->digest.words[3] = digest[3];
#else
#define FINGERPRINT_BYTE_STEP(word, value) do { \
	(word) ^= (uint32_t) (value); \
	(word) *= UINT32_C(16777619); \
} while (0)
	for (index = 0; index < length; index++) {
		const uint32_t value = (unsigned char) bytes[index];
		uint32_t *words = writer->digest.words;

		FINGERPRINT_BYTE_STEP(words[0], value ^ UINT32_C(0x88));
		FINGERPRINT_BYTE_STEP(words[0], UINT32_C(0x6a));
		FINGERPRINT_BYTE_STEP(words[0], UINT32_C(0x3f));
		FINGERPRINT_BYTE_STEP(words[0], UINT32_C(0x24));
		FINGERPRINT_BYTE_STEP(words[1], value ^ UINT32_C(0xd3));
		FINGERPRINT_BYTE_STEP(words[1], UINT32_C(0x08));
		FINGERPRINT_BYTE_STEP(words[1], UINT32_C(0xa3));
		FINGERPRINT_BYTE_STEP(words[1], UINT32_C(0x85));
		FINGERPRINT_BYTE_STEP(words[2], value ^ UINT32_C(0x2e));
		FINGERPRINT_BYTE_STEP(words[2], UINT32_C(0x8a));
		FINGERPRINT_BYTE_STEP(words[2], UINT32_C(0x19));
		FINGERPRINT_BYTE_STEP(words[2], UINT32_C(0x13));
		FINGERPRINT_BYTE_STEP(words[3], value ^ UINT32_C(0x44));
		FINGERPRINT_BYTE_STEP(words[3], UINT32_C(0x73));
		FINGERPRINT_BYTE_STEP(words[3], UINT32_C(0x70));
		FINGERPRINT_BYTE_STEP(words[3], UINT32_C(0x03));
	}
#undef FINGERPRINT_BYTE_STEP
#endif
	return true;
}

static zend_mir_fingerprint_digest
zend_mir_fingerprint_source_operand(
	zend_mir_fingerprint_digest digest,
	const zend_mir_source_operand_ref *operand)
{
	digest = zend_mir_fingerprint_mix(
		digest, (uint32_t) operand->kind);
	digest = zend_mir_fingerprint_mix(
		digest, (uint32_t) operand->slot_kind);
	digest = zend_mir_fingerprint_mix(digest, operand->index);
	return zend_mir_fingerprint_mix(
		digest, operand->ssa_variable_id);
}

static bool zend_mir_build_fingerprints(
	const zend_mir_view *view,
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics,
	uint32_t module_fingerprint[4],
	uint32_t source_fingerprint[4])
{
	zend_mir_call_fingerprint_writer module_writer;
	zend_mir_text_writer writer;
	zend_mir_fingerprint_digest source_seed =
		zend_mir_fingerprint_init();
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
	module_writer.digest = zend_mir_fingerprint_init();
	writer.context = &module_writer;
	writer.write = zend_mir_fingerprint_write;
	if (!zend_mir_dump_text(view, &writer, diagnostics)) {
		return false;
	}

#define MIX(record, field) \
	source_seed = zend_mir_fingerprint_mix_u64( \
		source_seed, (uint64_t) (record).field)
#define MIX_OPERAND(record, field) \
	source_seed = zend_mir_fingerprint_source_operand( \
		source_seed, &(record).field)

	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->opcode_count(source->context));
	for (index = 0; index < source->opcode_count(source->context); index++) {
		zend_mir_source_opcode_ref record;
		if (!source->opcode_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, opline_index);
		MIX(record, zend_opcode_number);
		MIX_OPERAND(record, op1);
		MIX_OPERAND(record, op2);
		MIX_OPERAND(record, result);
		MIX(record, extended_value);
		MIX(record, source_position_id);
		MIX(record, block_id);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->ssa_count(source->context));
	for (index = 0; index < source->ssa_count(source->context); index++) {
		zend_mir_source_ssa_ref record;
		if (!source->ssa_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, ssa_variable_id);
		MIX(record, definition_opline_index);
		MIX(record, source_slot);
		MIX(record, source_slot_kind);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->ssa_use_count(source->context));
	for (index = 0; index < source->ssa_use_count(source->context); index++) {
		zend_mir_source_ssa_use_ref record;
		if (!source->ssa_use_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, ssa_variable_id);
		MIX(record, opline_index);
		MIX(record, operand_index);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->ssa_def_count(source->context));
	for (index = 0; index < source->ssa_def_count(source->context); index++) {
		zend_mir_source_ssa_def_ref record;
		if (!source->ssa_def_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, ssa_variable_id);
		MIX(record, opline_index);
		MIX_OPERAND(record, destination);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->literal_count(source->context));
	for (index = 0; index < source->literal_count(source->context); index++) {
		zend_mir_source_literal_ref record;
		if (!source->literal_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, literal_index);
		MIX(record, kind);
		MIX(record, payload_bits);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->block_count(source->context));
	for (index = 0; index < source->block_count(source->context); index++) {
		zend_mir_source_block_ref record;
		if (!source->block_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, first_opcode_ordinal);
		MIX(record, opcode_count);
		MIX(record, flags);
		MIX(record, immediate_dominator);
		MIX(record, loop_header);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->edge_count(source->context));
	for (index = 0; index < source->edge_count(source->context); index++) {
		zend_mir_source_edge_ref record;
		if (!source->edge_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, from_block_id);
		MIX(record, to_block_id);
		MIX(record, successor_index);
		MIX(record, predecessor_index);
		MIX(record, flags);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->phi_count(source->context));
	for (index = 0; index < source->phi_count(source->context); index++) {
		zend_mir_source_phi_ref record;
		if (!source->phi_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, block_id);
		MIX(record, result_ssa_variable_id);
		MIX(record, source_slot_kind);
		MIX(record, source_slot_index);
		MIX(record, kind);
		MIX(record, constraint.type_mask);
		MIX(record, constraint.range_min);
		MIX(record, constraint.range_max);
		MIX(record, constraint.min_ssa_variable_id);
		MIX(record, constraint.max_ssa_variable_id);
		MIX(record, constraint.flags);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source->phi_input_count(source->context));
	for (index = 0;
			index < source->phi_input_count(source->context); index++) {
		zend_mir_source_phi_input_ref record;
		if (!source->phi_input_at(source->context, index, &record)) {
			return false;
		}
		MIX(record, phi_id);
		MIX(record, input_index);
		MIX(record, predecessor_block_id);
		MIX(record, source_ssa_variable_id);
	}

	source_seed = zend_mir_fingerprint_mix(
		source_seed, source_calls->source_opcode_count(source_calls->context));
	for (index = 0;
			index < source_calls->source_opcode_count(source_calls->context);
			index++) {
		zend_mir_source_opcode_ref record;
		if (!source_calls->source_opcode_at(
				source_calls->context, index, &record)) {
			return false;
		}
		MIX(record, opline_index);
		MIX(record, zend_opcode_number);
		MIX_OPERAND(record, op1);
		MIX_OPERAND(record, op2);
		MIX_OPERAND(record, result);
		MIX(record, extended_value);
		MIX(record, source_position_id);
		MIX(record, block_id);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source_calls->call_site_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_site_count(source_calls->context);
			index++) {
		zend_mir_source_call_site_ref record;
		if (!source_calls->call_site_at(
				source_calls->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, parent_call_site_id);
		MIX(record, init_opline_index);
		MIX(record, do_opline_index);
		MIX(record, source_block_id);
		MIX(record, target_id);
		MIX(record, argument_span.offset);
		MIX(record, argument_span.count);
		MIX(record, result_ssa_variable_id);
		MIX_OPERAND(record, result_operand);
		MIX(record, flags);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source_calls->call_target_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_target_count(source_calls->context);
			index++) {
		zend_mir_source_call_target_ref record;
		if (!source_calls->call_target_at(
				source_calls->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, kind);
		MIX(record, function_symbol_id);
		MIX(record, op_array_id);
		MIX(record, num_args);
		MIX(record, required_num_args);
		MIX(record, function_flags_snapshot);
		MIX(record, parameter_modes.offset);
		MIX(record, parameter_modes.count);
		MIX(record, variadic);
		MIX(record, returns_by_reference);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source_calls->call_argument_count(source_calls->context));
	for (index = 0;
			index < source_calls->call_argument_count(source_calls->context);
			index++) {
		zend_mir_source_call_argument_ref record;
		if (!source_calls->call_argument_at(
				source_calls->context, index, &record)) {
			return false;
		}
		MIX(record, id);
		MIX(record, call_site_id);
		MIX(record, send_opline_index);
		MIX(record, ordinal);
		MIX(record, name_symbol_id);
		MIX(record, mode);
		MIX(record, flags);
		MIX(record, value_ssa_variable_id);
		MIX_OPERAND(record, source_operand);
	}
	source_seed = zend_mir_fingerprint_mix(
		source_seed, source_calls->parameter_mode_count(source_calls->context));
	for (index = 0;
			index < source_calls->parameter_mode_count(source_calls->context);
			index++) {
		zend_mir_source_parameter_mode_ref record;
		if (!source_calls->parameter_mode_at(
				source_calls->context, index, &record)) {
			return false;
		}
		MIX(record, target_id);
		MIX(record, ordinal);
		MIX(record, mode);
	}
#undef MIX_OPERAND
#undef MIX

	module_writer.digest = zend_mir_fingerprint_mix(
		module_writer.digest, ZEND_MIR_CONTRACT_VERSION);
	source_seed = zend_mir_fingerprint_mix(
		source_seed, ZEND_MIR_CONTRACT_VERSION);
	memcpy(module_fingerprint, module_writer.digest.words,
		sizeof(module_writer.digest.words));
	memcpy(source_fingerprint, source_seed.words,
		sizeof(source_seed.words));
	return true;
}

static bool zend_mir_words_equal(
	const uint32_t left[4], const uint32_t right[4])
{
	return memcmp(left, right, 4 * sizeof(uint32_t)) == 0;
}
#endif

#if !defined(NDEBUG)
typedef struct _zend_mir_final_indexes {
	zend_mir_id_index_entry *functions;
	zend_mir_id_index_entry *blocks;
	zend_mir_id_index_entry *values;
	zend_mir_id_index_entry *facts;
	zend_mir_id_index_entry *frames;
	zend_mir_id_index_entry *sources;
	uint32_t function_capacity;
	uint32_t block_capacity;
	uint32_t value_capacity;
	uint32_t fact_capacity;
	uint32_t frame_capacity;
	uint32_t source_capacity;
} zend_mir_final_indexes;

static void zend_mir_final_indexes_destroy(zend_mir_final_indexes *indexes)
{
	free(indexes->sources);
	free(indexes->frames);
	free(indexes->facts);
	free(indexes->values);
	free(indexes->blocks);
	free(indexes->functions);
	memset(indexes, 0, sizeof(*indexes));
}

static bool zend_mir_final_indexes_initialize(
		const zend_mir_view *view, zend_mir_final_indexes *indexes)
{
	uint32_t function_count;
	uint32_t block_count;
	uint32_t value_count;
	uint32_t fact_count;
	uint32_t frame_count;
	uint32_t source_count;
	uint32_t index;

	memset(indexes, 0, sizeof(*indexes));
	if (view == NULL
			|| view->function_count == NULL || view->function_at == NULL
			|| view->block_count == NULL || view->block_at == NULL
			|| view->value_count == NULL || view->value_at == NULL
			|| view->value_fact_count == NULL || view->value_fact_at == NULL
			|| view->frame_state_count == NULL || view->frame_state_at == NULL
			|| view->source_position_count == NULL
			|| view->source_position_at == NULL) {
		return false;
	}
	function_count = view->function_count(view->context);
	block_count = view->block_count(view->context);
	value_count = view->value_count(view->context);
	fact_count = view->value_fact_count(view->context);
	frame_count = view->frame_state_count(view->context);
	source_count = view->source_position_count(view->context);
	if (!zend_mir_id_index_capacity(function_count, &indexes->function_capacity)
			|| !zend_mir_id_index_capacity(block_count, &indexes->block_capacity)
			|| !zend_mir_id_index_capacity(value_count, &indexes->value_capacity)
			|| !zend_mir_id_index_capacity(fact_count, &indexes->fact_capacity)
			|| !zend_mir_id_index_capacity(frame_count, &indexes->frame_capacity)
			|| !zend_mir_id_index_capacity(source_count, &indexes->source_capacity)) {
		return false;
	}
	indexes->functions = zend_mir_call_calloc(indexes->function_capacity,
		sizeof(*indexes->functions), ZEND_MIR_ALLOCATION_PLANNER);
	indexes->blocks = zend_mir_call_calloc(indexes->block_capacity,
		sizeof(*indexes->blocks), ZEND_MIR_ALLOCATION_PLANNER);
	indexes->values = zend_mir_call_calloc(indexes->value_capacity,
		sizeof(*indexes->values), ZEND_MIR_ALLOCATION_PLANNER);
	indexes->facts = zend_mir_call_calloc(indexes->fact_capacity,
		sizeof(*indexes->facts), ZEND_MIR_ALLOCATION_PLANNER);
	indexes->frames = zend_mir_call_calloc(indexes->frame_capacity,
		sizeof(*indexes->frames), ZEND_MIR_ALLOCATION_PLANNER);
	indexes->sources = zend_mir_call_calloc(indexes->source_capacity,
		sizeof(*indexes->sources), ZEND_MIR_ALLOCATION_PLANNER);
	if (indexes->functions == NULL || indexes->blocks == NULL
			|| indexes->values == NULL || indexes->facts == NULL
			|| indexes->frames == NULL || indexes->sources == NULL) {
		zend_mir_final_indexes_destroy(indexes);
		return false;
	}
	for (index = 0; index < function_count; index++) {
		zend_mir_function_record record;
		if (!view->function_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->functions,
					indexes->function_capacity, record.id, index, NULL)) {
			goto failure;
		}
	}
	for (index = 0; index < block_count; index++) {
		zend_mir_block_record record;
		if (!view->block_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->blocks,
					indexes->block_capacity, record.id, index, NULL)) {
			goto failure;
		}
	}
	for (index = 0; index < value_count; index++) {
		zend_mir_value_record record;
		if (!view->value_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->values,
					indexes->value_capacity, record.id, index, NULL)) {
			goto failure;
		}
	}
	for (index = 0; index < fact_count; index++) {
		zend_mir_value_fact_ref record;
		if (!view->value_fact_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->facts,
					indexes->fact_capacity, record.value_id, index, NULL)) {
			goto failure;
		}
	}
	for (index = 0; index < frame_count; index++) {
		zend_mir_frame_state_ref record;
		if (!view->frame_state_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->frames,
					indexes->frame_capacity, record.id, index, NULL)) {
			goto failure;
		}
	}
	for (index = 0; index < source_count; index++) {
		zend_mir_source_position_ref record;
		if (!view->source_position_at(view->context, index, &record)
				|| !zend_mir_id_index_insert(indexes->sources,
					indexes->source_capacity, record.id, index, NULL)) {
			goto failure;
		}
	}
	return true;

failure:
	zend_mir_final_indexes_destroy(indexes);
	return false;
}

static bool zend_mir_verify_scalar_result_indexed(
		const zend_mir_view *view, const zend_mir_final_indexes *indexes,
		zend_mir_value_id value_id, zend_mir_representation *representation_out)
{
	zend_mir_value_record value;
	zend_mir_value_fact_ref fact;
	int32_t value_index = zend_mir_id_index_find(
		indexes->values, indexes->value_capacity, value_id);
	int32_t fact_index = zend_mir_id_index_find(
		indexes->facts, indexes->fact_capacity, value_id);

	if (value_index < 0 || fact_index < 0
			|| !view->value_at(view->context, (uint32_t) value_index, &value)
			|| !view->value_fact_at(view->context, (uint32_t) fact_index, &fact)
			|| !zend_mir_scalar_type_is_exact(fact.exact_type)
			|| (fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
		return false;
	}
	*representation_out = value.representation;
	return true;
}

static bool zend_mir_verify_machine_result_indexed(
		const zend_mir_view *view, const zend_mir_final_indexes *indexes,
		zend_mir_value_id value_id, zend_mir_representation *representation_out)
{
	zend_mir_value_record value;
	int32_t index = zend_mir_id_index_find(
		indexes->values, indexes->value_capacity, value_id);

	if (index < 0 || !view->value_at(view->context, (uint32_t) index, &value)
			|| value.representation == ZEND_MIR_REPRESENTATION_VOID) {
		return false;
	}
	*representation_out = value.representation;
	return true;
}
#endif

static bool zend_mir_verify_frame_shape(
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
		&& zend_mir_span_equal(frame->slots, descriptor->slots)
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

#if !defined(NDEBUG)
static bool zend_mir_call_find_function(
	const zend_mir_view *view, const zend_mir_final_indexes *indexes,
	zend_mir_function_id id,
	zend_mir_function_record *out)
{
	zend_mir_function_record record;
	int32_t index = zend_mir_id_index_find(
		indexes->functions, indexes->function_capacity, id);
	if (index < 0
			|| !view->function_at(view->context, (uint32_t) index, &record)
			|| record.id != id) {
		return false;
	}
	if (out != NULL) {
		*out = record;
	}
	return true;
}

static bool zend_mir_call_find_block(
	const zend_mir_view *view, const zend_mir_final_indexes *indexes,
	zend_mir_block_id id,
	zend_mir_block_record *out)
{
	zend_mir_block_record record;
	int32_t index = zend_mir_id_index_find(
		indexes->blocks, indexes->block_capacity, id);
	if (index < 0
			|| !view->block_at(view->context, (uint32_t) index, &record)
			|| record.id != id) {
		return false;
	}
	if (out != NULL) {
		*out = record;
	}
	return true;
}

static bool zend_mir_verify_final_structural(
	const zend_mir_view *view, const zend_mir_final_indexes *indexes,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (view == NULL
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
				|| !zend_mir_call_find_block(
					view, indexes, function.entry_block_id, &entry)
				|| entry.function_id != function.id) {
			goto failure;
		}
	}
	for (index = 0; index < view->block_count(view->context); index++) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, index, &block)
				|| !zend_mir_call_find_function(
					view, indexes, block.function_id, NULL)) {
			goto failure;
		}
	}
	for (index = 0;
		index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record instruction;
		uint32_t operand_index;
		uint32_t operand_count;
		if (!view->instruction_at(view->context, index, &instruction)
				|| !zend_mir_call_find_block(
					view, indexes, instruction.block_id, NULL)
				|| (uint32_t) instruction.opcode
					>= (ZEND_MIR_OPCODE_COUNT)
				|| (uint32_t) instruction.representation
					>= ZEND_MIR_REPRESENTATION_COUNT
				|| (zend_mir_id_is_valid(instruction.result_id)
					&& zend_mir_id_index_find(indexes->values,
						indexes->value_capacity, instruction.result_id) < 0)
				|| (zend_mir_id_is_valid(instruction.frame_state_id)
					&& zend_mir_id_index_find(indexes->frames,
						indexes->frame_capacity, instruction.frame_state_id) < 0)
				|| (zend_mir_id_is_valid(
						instruction.source_position_id)
					&& zend_mir_id_index_find(indexes->sources,
						indexes->source_capacity,
						instruction.source_position_id) < 0)) {
			goto failure;
		}
		operand_count = view->instruction_operand_count(
			view->context, instruction.id);
		for (operand_index = 0;
			operand_index < operand_count; operand_index++) {
			zend_mir_value_id operand;
			if (!view->instruction_operand_at(
					view->context, instruction.id,
					operand_index, &operand)
					|| zend_mir_id_index_find(indexes->values,
						indexes->value_capacity, operand) < 0) {
				goto failure;
			}
		}
	}
	return true;

failure:
	zend_mir_call_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_SITE_MISMATCH,
		ZEND_MIRV_TOKEN_SITE_MISMATCH);
	return false;
}

static bool zend_mir_verify_final_scalar(
	const zend_mir_view *view, const zend_mir_final_indexes *indexes,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (view == NULL || calls == NULL
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
				|| (argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
					? (!zend_mir_verify_scalar_result_indexed(
							view, indexes, argument.value_id, &representation)
						|| representation == ZEND_MIR_REPRESENTATION_VOID)
					: ((zend_mir_id_is_valid(argument.value_id)
							? (argument.ownership
									!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
								|| !zend_mir_verify_machine_result_indexed(
									view, indexes, argument.value_id,
									&representation))
							: false)
						|| (argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
							&& argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE)))) {
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
				&& ((!zend_mir_verify_scalar_result_indexed(
							view, indexes, site.result_id, &representation)
						|| representation
							== ZEND_MIR_REPRESENTATION_VOID)
					&& (!zend_mir_verify_machine_result_indexed(
							view, indexes, site.result_id, &representation)))) {
			goto failure;
		}
	}
	return true;

failure:
	zend_mir_call_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_ARGUMENT_MISMATCH,
		ZEND_MIRV_TOKEN_ARGUMENT_MISMATCH);
	return false;
}

static bool zend_mir_block_has_predecessor(
	const zend_mir_view *view, zend_mir_block_id block_id,
	zend_mir_block_id predecessor)
{
	uint32_t index;
	uint32_t count = view->predecessor_count(view->context, block_id);
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

static bool zend_mir_block_has_successor(
	const zend_mir_view *view, zend_mir_block_id block_id,
	zend_mir_block_id successor)
{
	uint32_t index;
	uint32_t count = view->successor_count(view->context, block_id);
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

static bool zend_mir_verify_final_control_flow(
	const zend_mir_view *view, const zend_mir_final_indexes *indexes,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t index;

	if (view == NULL || calls == NULL
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
		/*
		 * Successor and predecessor lists describe CFG edges, not unique
		 * blocks.  Switch-like opcodes may legitimately contribute more
		 * parallel edges than the module has blocks.
		 */
		for (edge_index = 0;
			edge_index < successor_count; edge_index++) {
			zend_mir_block_id successor;
			if (!view->successor_at(
					view->context, block.id, edge_index, &successor)
					|| !zend_mir_call_find_block(
						view, indexes, successor, NULL)
					|| !zend_mir_block_has_predecessor(
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
					|| !zend_mir_call_find_block(
						view, indexes, predecessor, NULL)
					|| !zend_mir_block_has_successor(
						view, predecessor, block.id)) {
				goto failure;
			}
		}
	}
	for (index = 0; index < calls->call_site_count(calls->context); index++) {
		zend_mir_call_site_ref site;
		zend_mir_call_continuation_ref continuation;
		zend_mir_call_target_ref target;
		zend_mir_instruction_record instruction;
		if (!calls->call_site_at(calls->context, index, &site)
				|| !view->instruction_at(
					view->context, site.instruction_id, &instruction)
				|| calls->call_target_at == NULL
				|| !calls->call_target_at(
					calls->context, site.target_id, &target)
				|| instruction.opcode !=
					(target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
						? (ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
						: ZEND_MIR_OPCODE_CALL_DIRECT_USER)
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
	zend_mir_call_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_CONTINUATION_MISMATCH,
		ZEND_MIRV_TOKEN_CONTINUATION_MISMATCH);
	return false;
}

static bool zend_mir_verify_calls(
	const zend_mir_view *view,
	const zend_mir_final_indexes *indexes,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t source_site_count;
	uint32_t source_target_count;
	uint32_t source_argument_count;
	uint32_t index;

	if (view == NULL || source_calls == NULL || calls == NULL
			|| source_calls->call_site_count == NULL
			|| source_calls->call_site_at == NULL
			|| source_calls->call_target_count == NULL
			|| source_calls->call_target_at == NULL
			|| source_calls->call_argument_count == NULL
			|| source_calls->call_argument_at == NULL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL
			|| calls->call_target_count == NULL || calls->call_target_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->call_continuation_at == NULL
			|| view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->frame_state_at == NULL
			|| view->frame_slot_at == NULL) {
		goto failure;
	}
	source_site_count = source_calls->call_site_count(source_calls->context);
	source_target_count =
		source_calls->call_target_count(source_calls->context);
	source_argument_count =
		source_calls->call_argument_count(source_calls->context);
	if (zend_mir_source_sequence(
			source_calls) != ZEND_MIRL_OK
			|| source_site_count == 0 || source_target_count == 0
			|| calls->call_site_count(calls->context) != source_site_count
			|| calls->call_target_count(calls->context) != source_target_count
			|| calls->call_argument_count(calls->context)
				!= source_argument_count) {
		goto failure;
	}
	for (index = 0; index < source_target_count; index++) {
		zend_mir_source_call_target_ref source;
		zend_mir_call_target_ref target;
		if (!source_calls->call_target_at(
				source_calls->context, index, &source)
				|| !calls->call_target_at(calls->context, index, &target)
				|| source.id != index || target.id != index
				|| target.kind !=
					(source.kind == ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL
						? ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
						: source.kind == ZEND_MIR_SOURCE_CALL_TARGET_METHOD
							? ZEND_MIR_CALL_TARGET_METHOD_USER
							: source.kind
								== ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER
									? ZEND_MIR_CALL_TARGET_DYNAMIC
									: ZEND_MIR_CALL_TARGET_DIRECT_USER)
				|| target.function_symbol_id != source.function_symbol_id
				|| target.op_array_id != source.op_array_id
				|| target.num_args != source.num_args
				|| target.required_num_args != source.required_num_args
				|| target.function_flags_snapshot
					!= source.function_flags_snapshot) {
			goto failure;
		}
	}
	for (index = 0; index < source_site_count; index++) {
		zend_mir_source_call_site_ref source;
		zend_mir_source_call_target_ref source_target;
		zend_mir_call_site_ref site;
		zend_mir_call_target_ref target;
		zend_mir_instruction_record instruction;
		zend_mir_frame_state_ref frame;
		zend_mir_representation result_representation;
		bool internal;
		bool scalar_arguments;
		uint32_t result_flags;
		uint32_t expected_operand_count;
		uint32_t argument_index;
		uint32_t continuation_index;

		if (!source_calls->call_site_at(
				source_calls->context, index, &source)
				|| source.target_id >= source_target_count
				|| !source_calls->call_target_at(source_calls->context,
					source.target_id, &source_target)
				|| !calls->call_site_at(calls->context, index, &site)
				|| !calls->call_target_at(
					calls->context, site.target_id, &target)
				|| !view->instruction_at(
					view->context, site.instruction_id, &instruction)) {
			goto failure;
		}
		internal = target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL;
		result_flags = source.flags
			& (ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED
				| ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
		scalar_arguments = !internal;
		for (argument_index = 0;
			scalar_arguments && argument_index < site.arguments.count;
			argument_index++) {
			zend_mir_call_argument_ref argument;
			if (!calls->call_argument_at(calls->context,
					site.arguments.offset + argument_index, &argument)
					|| argument.ownership
						!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR) {
				scalar_arguments = false;
			}
		}
		expected_operand_count = scalar_arguments
			? site.arguments.count : 0;
		result_representation = ZEND_MIR_REPRESENTATION_VOID;
		if ((view->instruction_operand_count(
						view->context, instruction.id)
						!= expected_operand_count
					|| (zend_mir_id_is_valid(site.result_id)
						? (!zend_mir_id_is_valid(
								source.result_ssa_variable_id)
							|| site.result_id != zend_mir_value_from_original_ssa(
								source.result_ssa_variable_id)
							|| instruction.result_id != site.result_id
							|| (result_flags
									== ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR
									? !zend_mir_verify_scalar_result_indexed(
										view, indexes, site.result_id,
									&result_representation)
								: (result_flags != 0
									|| !zend_mir_verify_machine_result_indexed(
											view, indexes, site.result_id,
										&result_representation)
									|| result_representation
										!= ZEND_MIR_REPRESENTATION_ZVAL))
							|| instruction.representation
								!= result_representation)
						: (zend_mir_id_is_valid(instruction.result_id)
							|| instruction.representation
								!= ZEND_MIR_REPRESENTATION_VOID)))) {
			goto failure;
		}
		if (site.id != index || site.source_call_site_id != source.id
				|| site.target_id != source.target_id
				|| site.arguments.offset != source.argument_span.offset
				|| site.arguments.count != source.argument_span.count
				|| memcmp(&site.result_operand, &source.result_operand,
					sizeof(site.result_operand)) != 0
				|| site.source_init_opline_index != source.init_opline_index
				|| site.source_do_opline_index != source.do_opline_index
				|| site.continuations.offset != index * 4
				|| site.continuations.count != 4
				|| instruction.opcode != (internal
					? ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
					: ZEND_MIR_OPCODE_CALL_DIRECT_USER)
				|| instruction.source_position_id != source.do_opline_index
				|| !view->frame_state_at(view->context,
					site.caller_frame.frame_state_id, &frame)
					|| !zend_mir_verify_frame_shape(
					&frame, &site.caller_frame, ZEND_MIR_ID_INVALID,
					source.do_opline_index,
					internal ? ZEND_MIR_SAFEPOINT_CLASS_INTERNAL_CALL
						: ZEND_MIR_SAFEPOINT_CLASS_USER_CALL)
					|| instruction.frame_state_id != frame.id) {
			goto failure;
		}
		for (argument_index = 0;
				argument_index < site.arguments.count; argument_index++) {
			zend_mir_source_call_argument_ref source_argument;
			zend_mir_call_argument_ref argument;
			zend_mir_frame_slot_ref caller_slot;
			zend_mir_frame_slot_ref callee_slot;
			if (!source_calls->call_argument_at(source_calls->context,
					source.argument_span.offset + argument_index,
					&source_argument)
					|| !calls->call_argument_at(calls->context,
						site.arguments.offset + argument_index, &argument)
					|| argument.id != source_argument.id
					|| argument.call_site_id != site.id
					|| argument.ordinal != argument_index
					|| argument.send_opline_index
						!= source_argument.send_opline_index
					|| argument.source_mode != source_argument.mode
					|| memcmp(&argument.source_operand,
						&source_argument.source_operand,
						sizeof(argument.source_operand)) != 0
					|| !view->frame_slot_at(view->context,
						site.caller_frame.slots.offset + argument_index,
						&caller_slot)
					|| !view->frame_slot_at(view->context,
						site.callee_entry_frame.slots.offset + argument_index,
						&callee_slot)) {
				goto failure;
			}
			zend_mir_source_parameter_mode parameter_mode;
			zend_mir_call_argument_ownership expected;
			bool value_source_mode;
			bool borrowed_scalar;
			bool boxed_machine_value;
			if (internal && (source_argument.mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
					|| source_argument.mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK)) {
				parameter_mode =
					ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
			} else if (internal
					&& source_argument.mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
				/* Named internal parameter modes are resolved against the
				 * actual zend_function while the backend builds its descriptor. */
				parameter_mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
			} else if (internal || source_target.kind
					== ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER) {
				if (argument_index >= source_target.num_args
						&& !source_target.variadic) {
					parameter_mode =
						ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
				} else if (!zend_mir_target_parameter_mode_at(
						source_calls, &source_target,
						argument_index, &parameter_mode)) {
					goto failure;
				}
			} else {
				parameter_mode = source_argument.mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
					? ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
					: ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
			}
			expected = parameter_mode
					== ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
				? ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
				: ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE;
			value_source_mode = source_argument.mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
				|| source_argument.mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED;
			borrowed_scalar = !internal
				&& parameter_mode
					== ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
				&& value_source_mode
				&& argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
			boxed_machine_value = !internal
				&& parameter_mode
					== ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
				&& value_source_mode
				&& argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
				&& zend_mir_id_is_valid(
					source_argument.value_ssa_variable_id)
				&& argument.value_id
					== zend_mir_value_from_original_ssa(
						source_argument.value_ssa_variable_id);
			if ((!borrowed_scalar && argument.ownership != expected)
					|| (borrowed_scalar
						? !zend_mir_id_is_valid(argument.value_id)
						: zend_mir_id_is_valid(argument.value_id)
							!= boxed_machine_value)
					|| caller_slot.materialization
						!= (borrowed_scalar
							? ZEND_MIR_MATERIALIZATION_MATERIALIZED
							: ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL)
					|| caller_slot.ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_CALLER_OWNED
					|| callee_slot.materialization
						!= (borrowed_scalar
							? ZEND_MIR_MATERIALIZATION_MATERIALIZED
							: ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL)
					|| callee_slot.ownership
						!= (borrowed_scalar
							? ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED
							: ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED)) {
				goto failure;
			}
		}
		for (continuation_index = 0; continuation_index < 4;
				continuation_index++) {
			zend_mir_call_continuation_ref continuation;
			bool protected_exception = continuation_index == 1
				&& (source.flags
					& ZEND_MIR_SOURCE_CALL_SITE_PROTECTED) != 0;
			if (!calls->call_continuation_at(calls->context,
					site.continuations.offset + continuation_index,
					&continuation)
					|| continuation.id
						!= site.continuations.offset + continuation_index
					|| continuation.call_site_id != site.id
					|| continuation.kind
						!= (zend_mir_call_continuation_kind) continuation_index
					|| (continuation_index == 0
						? continuation.block_id != instruction.block_id
							|| zend_mir_id_is_valid(
								continuation.source_opline_index)
						: protected_exception
							? !zend_mir_id_is_valid(continuation.block_id)
								|| !zend_mir_id_is_valid(
									continuation.source_opline_index)
							: zend_mir_id_is_valid(continuation.block_id)
								|| zend_mir_id_is_valid(
									continuation.source_opline_index))) {
				goto failure;
			}
		}
	}
	return true;

failure:
	zend_mir_call_verify_emit(diagnostics,
		ZEND_MIR_VERIFY_SITE_MISMATCH,
		ZEND_MIRV_TOKEN_SITE_MISMATCH);
	return false;
}
#endif

#if !defined(NDEBUG)
static bool zend_mir_verify_final_composition(
	const zend_mir_view *view,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_call_view *calls,
	zend_mir_diagnostic_sink *diagnostics)
{
	bool empty_calls;
	bool valid;
	zend_mir_final_indexes indexes;

	if (!zend_mir_final_indexes_initialize(view, &indexes)) {
		zend_mir_call_verify_emit(diagnostics,
			ZEND_MIR_VERIFY_SITE_MISMATCH,
			ZEND_MIRV_TOKEN_SITE_MISMATCH);
		return false;
	}
	if (!zend_mir_verify_final_structural(
			view, &indexes, diagnostics)) {
		valid = false;
		goto done;
	}
	if (!zend_mir_verify_final_scalar(
			view, &indexes, calls, diagnostics)) {
		valid = false;
		goto done;
	}
	if (!zend_mir_verify_final_control_flow(
			view, &indexes, calls, diagnostics)) {
		valid = false;
		goto done;
	}
	empty_calls = source_calls->call_site_count(source_calls->context) == 0
		&& source_calls->call_target_count(source_calls->context) == 0
		&& source_calls->call_argument_count(source_calls->context) == 0;
	valid = (empty_calls
			? calls->call_site_count(calls->context) == 0
				&& calls->call_target_count(calls->context) == 0
				&& calls->call_argument_count(calls->context) == 0
			: zend_mir_verify_calls(
					view, &indexes, source_calls, calls, diagnostics));
done:
	zend_mir_final_indexes_destroy(&indexes);
	return valid;
}
#endif

static zend_mir_lowering_result zend_mir_lower_direct_user_calls(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator)
{
	zend_mir_plan plan;
	zend_mir_lowering_result lowered;
	zend_mir_lowering_diagnostic_code code;
#if !defined(NDEBUG)
	const zend_mir_view *view;
	const zend_mir_call_view *calls;
	uint32_t module_fingerprint[4];
	uint32_t source_fingerprint[4];
	uint32_t recomputed_module_fingerprint[4];
	uint32_t recomputed_source_fingerprint[4];
#endif
	zend_mir_lowering_result result;

	code = zend_mir_plan_calls(
		context, source_calls, resolver, &plan,
		true);
	if (code != ZEND_MIRL_OK) {
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			code == ZEND_MIRL_CALL_PLAN_FAILED
				|| code == ZEND_MIRL_MALFORMED_CALL_SEQUENCE
				? ZEND_MIR_LOWERING_FAILED : ZEND_MIR_LOWERING_DEFERRED,
			code);
	}
	lowered = zend_mir_lower_control_flow_zend_source(context, mutator, control_flow_map);
	if (lowered.status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_plan_release(&plan);
		result = zend_mir_failure(lowered.status, lowered.diagnostic_code);
		return result;
	}
	if (!zend_mir_lowering_result_is_failure_atomic(
			&lowered, ZEND_MIR_LOWERING_GUARANTEE_VERIFIED)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_CALL_VERIFY_FAILED);
	}
	if (call_mutator == NULL) {
		call_mutator = zend_mir_module_get_call_mutator(lowered.module);
	}
	if (!zend_mir_emit_calls(
			&plan, context, call_mutator, true)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_CALL_PLAN_FAILED);
	}
	if (context->post_call_composition != NULL
			&& !context->post_call_composition(
				context->post_call_composition_context, context, lowered.module,
				control_flow_map)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED);
	}
	if (!context->module_ops.finalize(
			context->module_ops.context, lowered.module)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_CALL_PLAN_FAILED);
	}
#if !defined(NDEBUG)
	view = context->module_ops.view(context->module_ops.context, lowered.module);
	calls = zend_mir_module_get_call_view(lowered.module);
	if (view == NULL || calls == NULL
			|| !zend_mir_verify_final_composition(
				view, source_calls, calls, context->diagnostics)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_CALL_VERIFY_FAILED);
	}
	/* Recompute the stable projection to catch mutation during verification. */
	if (!zend_mir_build_fingerprints(
				view, context->source, source_calls, calls,
				context->diagnostics,
				module_fingerprint, source_fingerprint)
			|| !zend_mir_build_fingerprints(
				view, context->source, source_calls, calls,
				context->diagnostics,
				recomputed_module_fingerprint,
				recomputed_source_fingerprint)
			|| !zend_mir_words_equal(
				module_fingerprint, recomputed_module_fingerprint)
			|| !zend_mir_words_equal(
				 source_fingerprint, recomputed_source_fingerprint)) {
		context->module_ops.destroy(context->module_ops.context, lowered.module);
		zend_mir_plan_release(&plan);
		return zend_mir_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_CALL_VERIFY_FAILED);
	}
#endif
	zend_mir_plan_release(&plan);
	result = lowered;
	result.guarantees = ZEND_MIR_LOWERING_GUARANTEE_FINALIZED;
	return result;
}

zend_mir_lowering_result zend_mir_lower_calls_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator)
{
	return zend_mir_lower_direct_user_calls(
		context, mutator, control_flow_map, source_calls, resolver,
		call_mutator);
}
