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

#include "zend_mir_verify_scalar.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/Native/MIR/Verify/zend_mir_verify.h"

typedef struct _zend_mir_scalar_verify_context {
	const zend_mir_view *view;
	zend_mir_diagnostic_sink *diagnostics;
	zend_mir_module_id module_id;
	bool valid;
	zend_mir_value_record *values;
	zend_mir_instruction_record *instructions;
	zend_mir_source_position_ref *sources;
	zend_mir_value_fact_ref *facts_by_value;
	zend_mir_value_fact_ref *facts_by_id;
	uint32_t value_count;
	uint32_t instruction_count;
	uint32_t source_count;
	uint32_t fact_count;
} zend_mir_scalar_verify_context;

#ifdef ZEND_MIR_VERIFY_TESTING
static uint32_t zend_mir_scalar_allocations_before_failure = UINT32_MAX;

void zend_mir_verify_scalar_test_fail_allocation_after(
		uint32_t successful_allocations)
{
	zend_mir_scalar_allocations_before_failure = successful_allocations;
}
#endif

const char *zend_mir_scalar_verify_code_name(zend_mir_scalar_verify_code code)
{
	switch (code) {
		case ZEND_MIR_SCALAR_VERIFY_INVALID_ARGUMENT: return "MIRV0600";
		case ZEND_MIR_SCALAR_VERIFY_INCOMPLETE_VIEW: return "MIRV0601";
		case ZEND_MIR_SCALAR_VERIFY_CAPACITY_EXCEEDED: return "MIRV0602";
		case ZEND_MIR_SCALAR_VERIFY_ALLOCATION_FAILED: return "MIRV0603";
		case ZEND_MIR_SCALAR_VERIFY_CALLBACK_FAILED: return "MIRV0604";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_FACT: return "MIRV0610";
		case ZEND_MIR_SCALAR_VERIFY_DUPLICATE_FACT_ID: return "MIRV0611";
		case ZEND_MIR_SCALAR_VERIFY_DUPLICATE_VALUE_FACT: return "MIRV0612";
		case ZEND_MIR_SCALAR_VERIFY_MISSING_FACT: return "MIRV0613";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_OPCODE: return "MIRV0620";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_OPERAND: return "MIRV0621";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_RESULT: return "MIRV0622";
		case ZEND_MIR_SCALAR_VERIFY_MISSING_PROOF: return "MIRV0623";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_EFFECTS: return "MIRV0624";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_OWNERSHIP: return "MIRV0625";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_SOURCE: return "MIRV0626";
		case ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE: return "MIRV0627";
		default: return "MIRV0699";
	}
}

static void zend_mir_scalar_verify_emit(
		zend_mir_scalar_verify_context *context,
		zend_mir_scalar_verify_code code,
		zend_mir_diagnostic_code generic_code,
		const zend_mir_instruction_record *instruction,
		zend_mir_value_id value_id, const char *message)
{
	zend_mir_diagnostic diagnostic;

	if (context != NULL) {
		context->valid = false;
	}
	if (context == NULL || context->diagnostics == NULL
			|| context->diagnostics->emit == NULL) {
		return;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = generic_code;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = context->module_id;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id =
		instruction != NULL ? instruction->block_id : ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id =
		instruction != NULL ? instruction->id : ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id =
		instruction != NULL ? instruction->frame_state_id : ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id =
		instruction != NULL
			? instruction->source_position_id : ZEND_MIR_ID_INVALID;
	if (zend_mir_id_is_valid(value_id)) {
		(void) snprintf(diagnostic.message, sizeof(diagnostic.message),
			"[%s] %s; value=v%u",
			zend_mir_scalar_verify_code_name(code), message, value_id);
	} else {
		(void) snprintf(diagnostic.message, sizeof(diagnostic.message),
			"[%s] %s", zend_mir_scalar_verify_code_name(code), message);
	}
	(void) zend_mir_diagnostic_sink_emit(context->diagnostics, &diagnostic);
}

static void *zend_mir_scalar_verify_allocate(
		zend_mir_scalar_verify_context *context,
		uint32_t count, size_t element_size)
{
	if (count == 0) {
		return NULL;
	}
	if (element_size == 0 || element_size > SIZE_MAX / count) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_CAPACITY_EXCEEDED,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED, NULL,
			ZEND_MIR_ID_INVALID, "scalar verifier allocation size overflow");
		return NULL;
	}
#ifdef ZEND_MIR_VERIFY_TESTING
	if (zend_mir_scalar_allocations_before_failure == 0) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_ALLOCATION_FAILED,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, NULL,
			ZEND_MIR_ID_INVALID, "scalar verifier allocation failed");
		return NULL;
	}
	zend_mir_scalar_allocations_before_failure--;
#endif
	{
		void *allocation = calloc((size_t) count, element_size);
		if (allocation == NULL) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_ALLOCATION_FAILED,
				ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, NULL,
				ZEND_MIR_ID_INVALID, "scalar verifier allocation failed");
		}
		return allocation;
	}
}

static int zend_mir_scalar_compare_value(const void *left, const void *right)
{
	const zend_mir_value_record *a = (const zend_mir_value_record *) left;
	const zend_mir_value_record *b = (const zend_mir_value_record *) right;

	return a->id < b->id ? -1 : a->id != b->id;
}

static int zend_mir_scalar_compare_instruction(const void *left, const void *right)
{
	const zend_mir_instruction_record *a =
		(const zend_mir_instruction_record *) left;
	const zend_mir_instruction_record *b =
		(const zend_mir_instruction_record *) right;

	return a->id < b->id ? -1 : a->id != b->id;
}

static int zend_mir_scalar_compare_source(const void *left, const void *right)
{
	const zend_mir_source_position_ref *a =
		(const zend_mir_source_position_ref *) left;
	const zend_mir_source_position_ref *b =
		(const zend_mir_source_position_ref *) right;

	return a->id < b->id ? -1 : a->id != b->id;
}

static int zend_mir_scalar_compare_fact_value(const void *left, const void *right)
{
	const zend_mir_value_fact_ref *a =
		(const zend_mir_value_fact_ref *) left;
	const zend_mir_value_fact_ref *b =
		(const zend_mir_value_fact_ref *) right;

	if (a->value_id != b->value_id) {
		return a->value_id < b->value_id ? -1 : 1;
	}
	return a->id < b->id ? -1 : a->id != b->id;
}

static int zend_mir_scalar_compare_fact_id(const void *left, const void *right)
{
	const zend_mir_value_fact_ref *a =
		(const zend_mir_value_fact_ref *) left;
	const zend_mir_value_fact_ref *b =
		(const zend_mir_value_fact_ref *) right;

	return a->id < b->id ? -1 : a->id != b->id;
}

static const zend_mir_value_record *zend_mir_scalar_find_value(
		const zend_mir_scalar_verify_context *context, zend_mir_value_id id)
{
	uint32_t left = 0;
	uint32_t right = context->value_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		if (context->values[middle].id < id) {
			left = middle + 1;
		} else if (context->values[middle].id > id) {
			right = middle;
		} else {
			return &context->values[middle];
		}
	}
	return NULL;
}

static const zend_mir_value_fact_ref *zend_mir_scalar_find_fact(
		const zend_mir_scalar_verify_context *context, zend_mir_value_id id)
{
	uint32_t left = 0;
	uint32_t right = context->fact_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		if (context->facts_by_value[middle].value_id < id) {
			left = middle + 1;
		} else if (context->facts_by_value[middle].value_id > id) {
			right = middle;
		} else {
			return &context->facts_by_value[middle];
		}
	}
	return NULL;
}

static bool zend_mir_scalar_has_source(
		const zend_mir_scalar_verify_context *context,
		zend_mir_source_position_id id)
{
	uint32_t left = 0;
	uint32_t right = context->source_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		if (context->sources[middle].id < id) {
			left = middle + 1;
		} else if (context->sources[middle].id > id) {
			right = middle;
		} else {
			return true;
		}
	}
	return false;
}

static bool zend_mir_scalar_load(zend_mir_scalar_verify_context *context)
{
	uint32_t index;

	context->value_count = context->view->value_count(context->view->context);
	context->instruction_count =
		context->view->instruction_count(context->view->context);
	context->source_count =
		context->view->source_position_count(context->view->context);
	context->fact_count =
		context->view->value_fact_count(context->view->context);
	if (context->value_count > ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT
			|| context->instruction_count > ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT
			|| context->source_count > ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT
			|| context->fact_count > ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_CAPACITY_EXCEEDED,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED, NULL,
			ZEND_MIR_ID_INVALID, "scalar entity count exceeds hard limit");
		return false;
	}
	context->values = zend_mir_scalar_verify_allocate(
		context, context->value_count, sizeof(*context->values));
	context->instructions = zend_mir_scalar_verify_allocate(
		context, context->instruction_count, sizeof(*context->instructions));
	context->sources = zend_mir_scalar_verify_allocate(
		context, context->source_count, sizeof(*context->sources));
	context->facts_by_value = zend_mir_scalar_verify_allocate(
		context, context->fact_count, sizeof(*context->facts_by_value));
	context->facts_by_id = zend_mir_scalar_verify_allocate(
		context, context->fact_count, sizeof(*context->facts_by_id));
	if ((context->value_count != 0 && context->values == NULL)
			|| (context->instruction_count != 0 && context->instructions == NULL)
			|| (context->source_count != 0 && context->sources == NULL)
			|| (context->fact_count != 0
				&& (context->facts_by_value == NULL
					|| context->facts_by_id == NULL))) {
		return false;
	}
	for (index = 0; index < context->value_count; index++) {
		if (!context->view->value_at(
				context->view->context, index, &context->values[index])) {
			goto callback_failed;
		}
	}
	for (index = 0; index < context->instruction_count; index++) {
		if (!context->view->instruction_at(
				context->view->context, index, &context->instructions[index])) {
			goto callback_failed;
		}
	}
	for (index = 0; index < context->source_count; index++) {
		if (!context->view->source_position_at(
				context->view->context, index, &context->sources[index])) {
			goto callback_failed;
		}
	}
	for (index = 0; index < context->fact_count; index++) {
		if (!context->view->value_fact_at(context->view->context, index,
				&context->facts_by_value[index])) {
			goto callback_failed;
		}
		context->facts_by_id[index] = context->facts_by_value[index];
	}
	if (context->value_count > 1) {
		qsort(context->values, context->value_count,
			sizeof(*context->values), zend_mir_scalar_compare_value);
	}
	if (context->instruction_count > 1) {
		qsort(context->instructions, context->instruction_count,
			sizeof(*context->instructions), zend_mir_scalar_compare_instruction);
	}
	if (context->source_count > 1) {
		qsort(context->sources, context->source_count,
			sizeof(*context->sources), zend_mir_scalar_compare_source);
	}
	if (context->fact_count > 1) {
		qsort(context->facts_by_value, context->fact_count,
			sizeof(*context->facts_by_value), zend_mir_scalar_compare_fact_value);
		qsort(context->facts_by_id, context->fact_count,
			sizeof(*context->facts_by_id), zend_mir_scalar_compare_fact_id);
	}
	return true;

callback_failed:
	zend_mir_scalar_verify_emit(context,
		ZEND_MIR_SCALAR_VERIFY_CALLBACK_FAILED,
		ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, NULL,
		ZEND_MIR_ID_INVALID, "scalar view callback failed");
	return false;
}

static void zend_mir_scalar_verify_facts(
		zend_mir_scalar_verify_context *context)
{
	uint32_t index;

	for (index = 0; index < context->fact_count; index++) {
		const zend_mir_value_fact_ref *fact = &context->facts_by_value[index];
		const zend_mir_value_record *value =
			zend_mir_scalar_find_value(context, fact->value_id);

		if (!zend_mir_id_is_valid(fact->id)
				|| !zend_mir_scalar_fact_is_well_formed(fact)
				|| value == NULL
				|| (value != NULL && value->representation
					!= zend_mir_scalar_type_representation(fact->exact_type))) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_INVALID_FACT,
				ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, NULL,
				fact->value_id, "value fact is malformed or mismatches its value");
		}
		if (index != 0
				&& context->facts_by_value[index - 1].value_id
					== fact->value_id) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_DUPLICATE_VALUE_FACT,
				ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID, NULL,
				fact->value_id, "value has more than one fact");
		}
		if (!zend_mir_id_is_valid(fact->provenance_source_position_id)
				|| !zend_mir_scalar_has_source(
					context, fact->provenance_source_position_id)) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_INVALID_SOURCE,
				ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, NULL,
				fact->value_id, "fact provenance source does not exist");
		}
	}
	for (index = 1; index < context->fact_count; index++) {
		if (context->facts_by_id[index - 1].id
				== context->facts_by_id[index].id) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_DUPLICATE_FACT_ID,
				ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID, NULL,
				context->facts_by_id[index].value_id,
				"value-fact ID is duplicated");
		}
	}
}

static void zend_mir_scalar_verify_scope(
		zend_mir_scalar_verify_context *context)
{
	zend_mir_block_record block;
	uint32_t return_count = 0;
	uint32_t index;

	if (!context->view->block_at(context->view->context, 0, &block)) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_CALLBACK_FAILED,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, NULL,
			ZEND_MIR_ID_INVALID, "scalar block callback failed");
		return;
	}
	if (context->view->successor_count(
			context->view->context, block.id) != 0
			|| context->view->predecessor_count(
				context->view->context, block.id) != 0) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE,
			ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE, NULL,
			ZEND_MIR_ID_INVALID,
			"W03 scalar block must be straight-line and have no CFG edges");
	}
	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_instruction_record *instruction =
			&context->instructions[index];

		switch (instruction->opcode) {
			case ZEND_MIR_OPCODE_PHI:
			case ZEND_MIR_OPCODE_BRANCH:
			case ZEND_MIR_OPCODE_COND_BRANCH:
			case ZEND_MIR_OPCODE_ITERATOR_BRANCH:
			case ZEND_MIR_OPCODE_THROW:
			case ZEND_MIR_OPCODE_UNREACHABLE:
				zend_mir_scalar_verify_emit(context,
					ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE,
					ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE,
					instruction, ZEND_MIR_ID_INVALID,
					"W03 scalar block contains deferred control-flow semantics");
				break;
			case ZEND_MIR_OPCODE_RETURN:
				return_count++;
				break;
			default:
				break;
		}
	}
	if (return_count != 1) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE,
			ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE, NULL,
			ZEND_MIR_ID_INVALID,
			"W03 scalar block must end in exactly one return");
	}
}

static bool zend_mir_scalar_requirement_matches(
		const zend_mir_scalar_value_requirement *requirement,
		const zend_mir_value_record *value,
		const zend_mir_value_fact_ref *fact)
{
	if (requirement == NULL || value == NULL
			|| !zend_mir_scalar_fact_is_well_formed(fact)) {
		return false;
	}
	if (requirement->representation != ZEND_MIR_REPRESENTATION_INVALID
			&& value->representation != requirement->representation) {
		return false;
	}
	if (requirement->exact_type != ZEND_MIR_SCALAR_TYPE_NONE
			&& fact->exact_type != requirement->exact_type) {
		return false;
	}
	return (fact->flags & requirement->required_flags)
			== requirement->required_flags
		&& value->ownership == requirement->ownership;
}

static bool zend_mir_scalar_add_safe(
		int64_t a_min, int64_t a_max, int64_t b_min, int64_t b_max)
{
	return !(b_min < 0 && a_min < INT64_MIN - b_min)
		&& !(b_max > 0 && a_max > INT64_MAX - b_max);
}

static bool zend_mir_scalar_sub_safe(
		int64_t a_min, int64_t a_max, int64_t b_min, int64_t b_max)
{
	return !(b_max > 0 && a_min < INT64_MIN + b_max)
		&& !(b_min < 0 && a_max > INT64_MAX + b_min);
}

static bool zend_mir_scalar_mul_pair_safe(int64_t left, int64_t right)
{
	if (left == 0 || right == 0) {
		return true;
	}
	if (left > 0) {
		return right > 0
			? left <= INT64_MAX / right
			: right >= INT64_MIN / left;
	}
	return right > 0
		? left >= INT64_MIN / right
		: left >= INT64_MAX / right;
}

static bool zend_mir_scalar_mul_safe(
		int64_t a_min, int64_t a_max, int64_t b_min, int64_t b_max)
{
	return zend_mir_scalar_mul_pair_safe(a_min, b_min)
		&& zend_mir_scalar_mul_pair_safe(a_min, b_max)
		&& zend_mir_scalar_mul_pair_safe(a_max, b_min)
		&& zend_mir_scalar_mul_pair_safe(a_max, b_max);
}

static void zend_mir_scalar_four_extrema(
		int64_t first, int64_t second, int64_t third, int64_t fourth,
		int64_t *minimum, int64_t *maximum)
{
	int64_t values[4];
	uint32_t index;

	values[0] = first;
	values[1] = second;
	values[2] = third;
	values[3] = fourth;
	*minimum = values[0];
	*maximum = values[0];
	for (index = 1; index < 4; index++) {
		if (values[index] < *minimum) {
			*minimum = values[index];
		}
		if (values[index] > *maximum) {
			*maximum = values[index];
		}
	}
}

static bool zend_mir_scalar_range_contains(
		const zend_mir_value_fact_ref *range, int64_t minimum, int64_t maximum)
{
	return range != NULL
		&& (range->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
		&& range->integer_min <= minimum && range->integer_max >= maximum;
}

static bool zend_mir_scalar_shift_count_valid(
		const zend_mir_value_fact_ref *shift)
{
	return shift != NULL
		&& (shift->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
		&& shift->integer_min >= 0 && shift->integer_max <= 63;
}

static bool zend_mir_scalar_shl_safe(
		const zend_mir_value_fact_ref *value,
		const zend_mir_value_fact_ref *shift)
{
	uint32_t maximum_shift = (uint32_t) shift->integer_max;
	int64_t factor;

	if (maximum_shift == 0) {
		return true;
	}
	if (maximum_shift == 63) {
		return value->integer_min >= -1 && value->integer_max <= 0;
	}
	factor = (int64_t) (UINT64_C(1) << maximum_shift);
	return value->integer_min >= INT64_MIN / factor
		&& value->integer_max <= INT64_MAX / factor;
}

static int64_t zend_mir_scalar_shl_value(int64_t value, uint32_t shift)
{
	if (shift == 63) {
		return value == -1 ? INT64_MIN : 0;
	}
	return value * (int64_t) (UINT64_C(1) << shift);
}

static int64_t zend_mir_scalar_shr_value(int64_t value, uint32_t shift)
{
	int64_t quotient;
	int64_t divisor;

	if (shift == 0) {
		return value;
	}
	if (shift == 63) {
		return value < 0 ? -1 : 0;
	}
	divisor = (int64_t) (UINT64_C(1) << shift);
	quotient = value / divisor;
	if (value < 0 && value % divisor != 0) {
		quotient--;
	}
	return quotient;
}

static bool zend_mir_scalar_result_range_proven(
		zend_mir_opcode opcode,
		const zend_mir_value_fact_ref *left,
		const zend_mir_value_fact_ref *right,
		const zend_mir_value_fact_ref *result)
{
	int64_t minimum;
	int64_t maximum;

	switch (opcode) {
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			if (!zend_mir_scalar_add_safe(left->integer_min,
					left->integer_max, right->integer_min,
					right->integer_max)) {
				return false;
			}
			return zend_mir_scalar_range_contains(result,
				left->integer_min + right->integer_min,
				left->integer_max + right->integer_max);
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			if (!zend_mir_scalar_sub_safe(left->integer_min,
					left->integer_max, right->integer_min,
					right->integer_max)) {
				return false;
			}
			return zend_mir_scalar_range_contains(result,
				left->integer_min - right->integer_max,
				left->integer_max - right->integer_min);
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			if (!zend_mir_scalar_mul_safe(left->integer_min,
					left->integer_max, right->integer_min,
					right->integer_max)) {
				return false;
			}
			zend_mir_scalar_four_extrema(
				left->integer_min * right->integer_min,
				left->integer_min * right->integer_max,
				left->integer_max * right->integer_min,
				left->integer_max * right->integer_max,
				&minimum, &maximum);
			return zend_mir_scalar_range_contains(result, minimum, maximum);
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO: {
			uint64_t a = right->integer_min == INT64_MIN
				? UINT64_C(1) << 63
				: (uint64_t) (right->integer_min < 0
					? -right->integer_min : right->integer_min);
			uint64_t b = right->integer_max == INT64_MIN
				? UINT64_C(1) << 63
				: (uint64_t) (right->integer_max < 0
					? -right->integer_max : right->integer_max);
			uint64_t magnitude = a > b ? a : b;

			if (right->integer_min <= 0 && right->integer_max >= 0) {
				return false;
			}
			if (left->integer_min == INT64_MIN
					&& right->integer_min <= -1
					&& right->integer_max >= -1) {
				return false;
			}
			maximum = magnitude > (uint64_t) INT64_MAX
				? INT64_MAX : (int64_t) magnitude - 1;
			minimum = -maximum;
			if (left->integer_min >= 0) {
				minimum = 0;
			}
			if (left->integer_max <= 0) {
				maximum = 0;
			}
			return zend_mir_scalar_range_contains(result, minimum, maximum);
		}
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED: {
			uint32_t low;
			uint32_t high;
			if (!zend_mir_scalar_shift_count_valid(right)
					|| !zend_mir_scalar_shl_safe(left, right)) {
				return false;
			}
			low = (uint32_t) right->integer_min;
			high = (uint32_t) right->integer_max;
			zend_mir_scalar_four_extrema(
				zend_mir_scalar_shl_value(left->integer_min, low),
				zend_mir_scalar_shl_value(left->integer_min, high),
				zend_mir_scalar_shl_value(left->integer_max, low),
				zend_mir_scalar_shl_value(left->integer_max, high),
				&minimum, &maximum);
			return zend_mir_scalar_range_contains(result, minimum, maximum);
		}
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED: {
			uint32_t low;
			uint32_t high;
			if (!zend_mir_scalar_shift_count_valid(right)) {
				return false;
			}
			low = (uint32_t) right->integer_min;
			high = (uint32_t) right->integer_max;
			zend_mir_scalar_four_extrema(
				zend_mir_scalar_shr_value(left->integer_min, low),
				zend_mir_scalar_shr_value(left->integer_min, high),
				zend_mir_scalar_shr_value(left->integer_max, low),
				zend_mir_scalar_shr_value(left->integer_max, high),
				&minimum, &maximum);
			return zend_mir_scalar_range_contains(result, minimum, maximum);
		}
		case ZEND_MIR_OPCODE_I64_CMP:
		case ZEND_MIR_OPCODE_F64_CMP:
			return zend_mir_scalar_range_contains(result, -1, 1);
		case ZEND_MIR_OPCODE_F64_ADD:
		case ZEND_MIR_OPCODE_F64_SUB:
		case ZEND_MIR_OPCODE_F64_MUL:
			return result->provenance
					== ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS
				|| result->provenance
					== ZEND_MIR_FACT_PROVENANCE_CONTRACT;
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return zend_mir_scalar_range_contains(result, 0, 1);
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED:
			return (result->provenance == ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS
				|| result->provenance == ZEND_MIR_FACT_PROVENANCE_CONTRACT);
		default:
			return true;
	}
}

static void zend_mir_scalar_verify_instruction(
		zend_mir_scalar_verify_context *context,
		const zend_mir_instruction_record *instruction,
		const zend_mir_scalar_descriptor *descriptor)
{
	const zend_mir_value_fact_ref *operand_facts[ZEND_MIR_SCALAR_MAX_OPERANDS] =
		{ NULL, NULL };
	bool operand_valid[ZEND_MIR_SCALAR_MAX_OPERANDS] = { false, false };
	const zend_mir_value_fact_ref *result_fact = NULL;
	bool result_valid = false;
	uint32_t operand_count;
	uint32_t index;

	if (instruction->effects != descriptor->effects
			|| instruction->reads != descriptor->reads
			|| instruction->writes != descriptor->writes
			|| instruction->barriers != descriptor->barriers) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_EFFECTS,
			ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS, instruction,
			ZEND_MIR_ID_INVALID,
			"scalar instruction effects must exactly match its descriptor");
	}
	if (instruction->ownership_actions != descriptor->ownership_actions) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_OWNERSHIP,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP, instruction,
			ZEND_MIR_ID_INVALID,
			"scalar instruction ownership actions must exactly match its descriptor");
	}
	if ((descriptor->requires_source
			&& (!zend_mir_id_is_valid(instruction->source_position_id)
				|| !zend_mir_scalar_has_source(
					context, instruction->source_position_id)))
			|| (!descriptor->requires_frame
				&& zend_mir_id_is_valid(instruction->frame_state_id))) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_SOURCE,
			ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, instruction,
			ZEND_MIR_ID_INVALID,
			"scalar instruction source or frame requirement is violated");
	}
	operand_count = context->view->instruction_operand_count(
		context->view->context, instruction->id);
	if (operand_count != descriptor->operand_count) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_OPERAND,
			ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, instruction,
			ZEND_MIR_ID_INVALID,
			"scalar instruction operand count is invalid");
		return;
	}
	for (index = 0; index < operand_count; index++) {
		zend_mir_value_id operand_id;
		const zend_mir_value_record *value;

		if (!context->view->instruction_operand_at(context->view->context,
				instruction->id, index, &operand_id)) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_CALLBACK_FAILED,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID, instruction,
				ZEND_MIR_ID_INVALID, "scalar operand callback failed");
			continue;
		}
		value = zend_mir_scalar_find_value(context, operand_id);
		operand_facts[index] = zend_mir_scalar_find_fact(context, operand_id);
		if (operand_facts[index] == NULL) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_MISSING_FACT,
				ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, instruction,
				operand_id, "scalar operand has no value fact");
		} else if (!zend_mir_scalar_requirement_matches(
				&descriptor->operands[index], value, operand_facts[index])) {
			zend_mir_scalar_verify_emit(context,
				value != NULL && value->ownership
						!= descriptor->operands[index].ownership
					? ZEND_MIR_SCALAR_VERIFY_INVALID_OWNERSHIP
					: ZEND_MIR_SCALAR_VERIFY_INVALID_OPERAND,
				value != NULL && value->ownership
						!= descriptor->operands[index].ownership
					? ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP
					: ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT,
				instruction, operand_id,
				"scalar operand representation, fact, or ownership is invalid");
		} else {
			operand_valid[index] = true;
		}
	}
	if (descriptor->has_result) {
		const zend_mir_value_record *result =
			zend_mir_scalar_find_value(context, instruction->result_id);
		result_fact = zend_mir_scalar_find_fact(context, instruction->result_id);
		if (result_fact == NULL) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_MISSING_FACT,
				ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, instruction,
				instruction->result_id, "scalar result has no value fact");
		} else if (instruction->representation
					!= descriptor->result.representation
				|| !zend_mir_scalar_requirement_matches(
					&descriptor->result, result, result_fact)) {
			zend_mir_scalar_verify_emit(context,
				result != NULL && result->ownership
						!= descriptor->result.ownership
					? ZEND_MIR_SCALAR_VERIFY_INVALID_OWNERSHIP
					: ZEND_MIR_SCALAR_VERIFY_INVALID_RESULT,
				result != NULL && result->ownership
						!= descriptor->result.ownership
					? ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP
					: ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT,
				instruction, instruction->result_id,
				"scalar result representation, fact, or ownership is invalid");
		} else {
			result_valid = true;
		}
	} else if (zend_mir_id_is_valid(instruction->result_id)
			|| instruction->representation != ZEND_MIR_REPRESENTATION_VOID) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_RESULT,
			ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE, instruction,
			instruction->result_id, "scalar drop must not define a result");
	}
	if (result_valid
			&& (descriptor->proofs
				& (ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW
					| ZEND_MIR_SCALAR_PROOF_NONZERO_DIVISOR
					| ZEND_MIR_SCALAR_PROOF_VALID_SHIFT_COUNT
					| ZEND_MIR_SCALAR_PROOF_RESULT_RANGE)) != 0
			&& operand_valid[0]
			&& (descriptor->operand_count < 2 || operand_valid[1])
			&& !zend_mir_scalar_result_range_proven(instruction->opcode,
				operand_facts[0], operand_facts[1], result_fact)) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_MISSING_PROOF,
			ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, instruction,
			instruction->result_id,
			"scalar range, nonzero, shift, or overflow proof is insufficient");
	}
}

static void zend_mir_scalar_verify_instructions(
		zend_mir_scalar_verify_context *context)
{
	uint32_t scalar_count = 0;
	uint32_t index;

	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_instruction_record *instruction =
			&context->instructions[index];
		const zend_mir_scalar_descriptor *descriptor =
			zend_mir_scalar_descriptor_at(instruction->opcode);

		if (descriptor == NULL) {
			continue;
		}
		if (descriptor->opcode != instruction->opcode) {
			zend_mir_scalar_verify_emit(context,
				ZEND_MIR_SCALAR_VERIFY_INVALID_OPCODE,
				ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE, instruction,
				ZEND_MIR_ID_INVALID,
				"scalar opcode has no exact descriptor");
			continue;
		}
		scalar_count++;
		zend_mir_scalar_verify_instruction(context, instruction, descriptor);
	}
	if (scalar_count == 0) {
		zend_mir_scalar_verify_emit(context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE,
			ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE, NULL,
			ZEND_MIR_ID_INVALID, "module contains no W03 scalar instruction");
	}
}

bool zend_mir_verify_w03_scalar(
		const zend_mir_view *view, zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_scalar_verify_context context;

	memset(&context, 0, sizeof(context));
	context.view = view;
	context.diagnostics = diagnostics;
	context.module_id = ZEND_MIR_ID_INVALID;
	context.valid = true;
	if (view == NULL) {
		zend_mir_scalar_verify_emit(&context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_ARGUMENT,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, NULL,
			ZEND_MIR_ID_INVALID, "view is null");
		goto done;
	}
	if (!zend_mir_verify_stage1(view, diagnostics)) {
		context.valid = false;
		goto done;
	}
	if (view->value_fact_count == NULL || view->value_fact_at == NULL) {
		zend_mir_scalar_verify_emit(&context,
			ZEND_MIR_SCALAR_VERIFY_INCOMPLETE_VIEW,
			ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT, NULL,
			ZEND_MIR_ID_INVALID, "value-fact callbacks are missing");
		goto done;
	}
	context.module_id = view->module_id(view->context);
	if (view->function_count(view->context) != 1
			|| view->block_count(view->context) != 1) {
		zend_mir_scalar_verify_emit(&context,
			ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE,
			ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE, NULL,
			ZEND_MIR_ID_INVALID,
			"W03 scalar module must contain exactly one function and block");
		goto done;
	}
	if (!zend_mir_scalar_load(&context)) {
		goto done;
	}
	zend_mir_scalar_verify_scope(&context);
	zend_mir_scalar_verify_facts(&context);
	zend_mir_scalar_verify_instructions(&context);

done:
	free(context.values);
	free(context.instructions);
	free(context.sources);
	free(context.facts_by_value);
	free(context.facts_by_id);
#ifdef ZEND_MIR_VERIFY_TESTING
	zend_mir_scalar_allocations_before_failure = UINT32_MAX;
#endif
	return context.valid;
}
