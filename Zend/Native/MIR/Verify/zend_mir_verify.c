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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _zend_mir_verify_code_entry {
	zend_mir_verify_code code;
	const char *name;
} zend_mir_verify_code_entry;

#define ZEND_MIR_VERIFY_CODE_ROW(code, number) \
	{ ZEND_MIR_VERIFY_##code, "MIRV" #number }

static const zend_mir_verify_code_entry zend_mir_verify_codes[] = {
	ZEND_MIR_VERIFY_CODE_ROW(OK, 0000),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_ARGUMENT, 0001),
	ZEND_MIR_VERIFY_CODE_ROW(ALLOCATION_FAILED, 0002),
	ZEND_MIR_VERIFY_CODE_ROW(DIAGNOSTIC_LIMIT, 0003),
	ZEND_MIR_VERIFY_CODE_ROW(UNSUPPORTED_VERSION, 0004),
	ZEND_MIR_VERIFY_CODE_ROW(INCOMPLETE_VIEW, 0005),
	ZEND_MIR_VERIFY_CODE_ROW(CALLBACK_FAILED, 0006),
	ZEND_MIR_VERIFY_CODE_ROW(CAPACITY_EXCEEDED, 0007),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_ID, 0100),
	ZEND_MIR_VERIFY_CODE_ROW(DUPLICATE_ID, 0101),
	ZEND_MIR_VERIFY_CODE_ROW(UNKNOWN_ENUM, 0102),
	ZEND_MIR_VERIFY_CODE_ROW(UNKNOWN_VALUE, 0103),
	ZEND_MIR_VERIFY_CODE_ROW(DUPLICATE_DEFINITION, 0104),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_CONSTANT, 0105),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_REPRESENTATION, 0106),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_FUNCTION, 0200),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_ENTRY, 0201),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_EDGE, 0202),
	ZEND_MIR_VERIFY_CODE_ROW(DUPLICATE_EDGE, 0203),
	ZEND_MIR_VERIFY_CODE_ROW(EDGE_MISMATCH, 0204),
	ZEND_MIR_VERIFY_CODE_ROW(MISSING_TERMINATOR, 0205),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_TERMINATOR, 0206),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_OPERAND_COUNT, 0207),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_PHI, 0208),
	ZEND_MIR_VERIFY_CODE_ROW(USE_BEFORE_DEFINITION, 0300),
	ZEND_MIR_VERIFY_CODE_ROW(DEFINITION_NOT_DOMINATING, 0301),
	ZEND_MIR_VERIFY_CODE_ROW(PHI_EDGE_NOT_DOMINATING, 0302),
	ZEND_MIR_VERIFY_CODE_ROW(UNKNOWN_SEMANTICS, 0400),
	ZEND_MIR_VERIFY_CODE_ROW(INCOMPLETE_SEMANTICS, 0401),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_OWNERSHIP, 0402),
	ZEND_MIR_VERIFY_CODE_ROW(TERMINAL_VALUE_USE, 0403),
	ZEND_MIR_VERIFY_CODE_ROW(DOUBLE_CONSUME, 0404),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_FRAME, 0500),
	ZEND_MIR_VERIFY_CODE_ROW(PARENT_CYCLE, 0501),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_SLOT, 0502),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_ROOT, 0503),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_CLEANUP, 0504),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_CONTINUATION, 0505),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_RESUME, 0506),
	ZEND_MIR_VERIFY_CODE_ROW(MISSING_FRAME, 0507),
	ZEND_MIR_VERIFY_CODE_ROW(FRAME_CLASS_MISMATCH, 0508),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_SOURCE, 0509),
	ZEND_MIR_VERIFY_CODE_ROW(MISSING_SOURCE, 0510),
	ZEND_MIR_VERIFY_CODE_ROW(INVALID_SOURCE_MAP, 0511),
};

#undef ZEND_MIR_VERIFY_CODE_ROW

#ifdef ZEND_MIR_VERIFY_TESTING
static uint32_t zend_mir_verify_allocations_before_failure = UINT32_MAX;

void zend_mir_verify_test_fail_allocation_after(uint32_t successful_allocations)
{
	zend_mir_verify_allocations_before_failure = successful_allocations;
}
#endif

const char *zend_mir_verify_code_name(zend_mir_verify_code code)
{
	uint32_t index;

	for (index = 0; index < sizeof(zend_mir_verify_codes) / sizeof(zend_mir_verify_codes[0]);
			index++) {
		if (zend_mir_verify_codes[index].code == code) {
			return zend_mir_verify_codes[index].name;
		}
	}
	return "MIRV9999";
}

static void zend_mir_verify_set_invalid(zend_mir_verify_context *context)
{
	if (context != NULL) {
		context->valid = false;
	}
}

static void zend_mir_verify_write_diagnostic(zend_mir_verify_context *context,
		zend_mir_verify_code verify_code, zend_mir_diagnostic_code generic_code,
		zend_mir_diagnostic_location location, zend_mir_value_id operand_id,
		const char *message, bool fatal)
{
	zend_mir_verify_pending_diagnostic *pending;
	const char *name;

	zend_mir_verify_set_invalid(context);
	if (context == NULL || context->diagnostics == NULL
			|| context->diagnostics->emit == NULL || context->diagnostic_capacity == 0) {
		return;
	}
	if (context->halted) {
		return;
	}
	if (!fatal && context->diagnostics_reported + 1 >= context->diagnostic_capacity) {
		verify_code = ZEND_MIR_VERIFY_DIAGNOSTIC_LIMIT;
		generic_code = ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED;
		location = zend_mir_verify_location();
		location.module_id = context->module_id;
		operand_id = ZEND_MIR_ID_INVALID;
		message = "diagnostic limit reached; remaining verifier errors suppressed";
		context->halted = true;
	}

	pending = &context->pending_diagnostics[context->diagnostics_reported];
	memset(pending, 0, sizeof(*pending));
	pending->verify_code = verify_code;
	pending->operand_id = operand_id;
	pending->sequence = context->diagnostics_reported;
	pending->diagnostic.code = generic_code;
	pending->diagnostic.severity =
		fatal ? ZEND_MIR_DIAGNOSTIC_FATAL : ZEND_MIR_DIAGNOSTIC_ERROR;
	pending->diagnostic.location = location;
	name = zend_mir_verify_code_name(verify_code);
	if (zend_mir_id_is_valid(operand_id)) {
		(void) snprintf(pending->diagnostic.message,
			sizeof(pending->diagnostic.message),
			"[%s] %s; operand=v%u", name, message, operand_id);
	} else {
		(void) snprintf(pending->diagnostic.message,
			sizeof(pending->diagnostic.message),
			"[%s] %s", name, message);
	}
	context->diagnostics_reported++;
	if (fatal) {
		context->halted = true;
	}
}

static int zend_mir_verify_compare_pending_diagnostic(
		const void *left, const void *right)
{
	const zend_mir_verify_pending_diagnostic *a =
		(const zend_mir_verify_pending_diagnostic *) left;
	const zend_mir_verify_pending_diagnostic *b =
		(const zend_mir_verify_pending_diagnostic *) right;
	const zend_mir_diagnostic_location *a_location = &a->diagnostic.location;
	const zend_mir_diagnostic_location *b_location = &b->diagnostic.location;
	uint32_t a_stage = (uint32_t) a->verify_code / 100;
	uint32_t b_stage = (uint32_t) b->verify_code / 100;

#define ZEND_MIR_VERIFY_COMPARE_FIELD(a_value, b_value) \
	do { \
		if ((a_value) < (b_value)) { \
			return -1; \
		} \
		if ((a_value) > (b_value)) { \
			return 1; \
		} \
	} while (0)

	ZEND_MIR_VERIFY_COMPARE_FIELD(a_stage, b_stage);
	ZEND_MIR_VERIFY_COMPARE_FIELD(
		a_location->function_id, b_location->function_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(a_location->block_id, b_location->block_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(
		a_location->instruction_id, b_location->instruction_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(
		a_location->frame_state_id, b_location->frame_state_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(
		a_location->source_position_id, b_location->source_position_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(a->verify_code, b->verify_code);
	ZEND_MIR_VERIFY_COMPARE_FIELD(a->operand_id, b->operand_id);
	ZEND_MIR_VERIFY_COMPARE_FIELD(a->sequence, b->sequence);

#undef ZEND_MIR_VERIFY_COMPARE_FIELD

	return 0;
}

static void zend_mir_verify_flush_diagnostics(zend_mir_verify_context *context)
{
	uint32_t index;

	if (context == NULL || context->diagnostics_reported == 0) {
		return;
	}
	if (context->diagnostics_reported > 1) {
		qsort(context->pending_diagnostics, context->diagnostics_reported,
			sizeof(context->pending_diagnostics[0]),
			zend_mir_verify_compare_pending_diagnostic);
	}
	for (index = 0; index < context->diagnostics_reported; index++) {
		if (!zend_mir_diagnostic_sink_emit(context->diagnostics,
				&context->pending_diagnostics[index].diagnostic)) {
			context->halted = true;
			break;
		}
	}
}

void zend_mir_verify_emit(zend_mir_verify_context *context,
		zend_mir_verify_code verify_code, zend_mir_diagnostic_code generic_code,
		zend_mir_diagnostic_location location, zend_mir_value_id operand_id,
		const char *message)
{
	zend_mir_verify_write_diagnostic(context, verify_code, generic_code,
		location, operand_id, message, false);
}

void zend_mir_verify_emit_fatal(zend_mir_verify_context *context,
		zend_mir_verify_code verify_code, zend_mir_diagnostic_code generic_code,
		const char *message)
{
	zend_mir_diagnostic_location location = zend_mir_verify_location();

	if (context != NULL) {
		location.module_id = context->module_id;
	}
	zend_mir_verify_write_diagnostic(context, verify_code, generic_code,
		location, ZEND_MIR_ID_INVALID, message, true);
}

void *zend_mir_verify_allocate(
		zend_mir_verify_context *context, uint32_t count, size_t element_size)
{
	zend_mir_verify_allocation *allocation;
	size_t bytes;

	if (context == NULL || count == 0) {
		return NULL;
	}
	if (element_size == 0 || element_size > SIZE_MAX / count
			|| sizeof(*allocation) > SIZE_MAX - element_size * count) {
		zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CAPACITY_EXCEEDED,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED, "scratch allocation size overflow");
		return NULL;
	}
#ifdef ZEND_MIR_VERIFY_TESTING
	if (zend_mir_verify_allocations_before_failure == 0) {
		zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_ALLOCATION_FAILED,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, "verifier scratch allocation failed");
		return NULL;
	}
	zend_mir_verify_allocations_before_failure--;
#endif
	bytes = element_size * count;
	allocation = (zend_mir_verify_allocation *) calloc(1, sizeof(*allocation) + bytes);
	if (allocation == NULL) {
		zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_ALLOCATION_FAILED,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, "verifier scratch allocation failed");
		return NULL;
	}
	allocation->next = context->allocations;
	context->allocations = allocation;
	return allocation->data;
}

void zend_mir_verify_release(zend_mir_verify_context *context)
{
	zend_mir_verify_allocation *allocation;

	if (context == NULL) {
		return;
	}
	allocation = context->allocations;
	while (allocation != NULL) {
		zend_mir_verify_allocation *next = allocation->next;

		free(allocation);
		allocation = next;
	}
	context->allocations = NULL;
}

zend_mir_diagnostic_location zend_mir_verify_location(void)
{
	zend_mir_diagnostic_location location;

	location.module_id = ZEND_MIR_ID_INVALID;
	location.function_id = ZEND_MIR_ID_INVALID;
	location.block_id = ZEND_MIR_ID_INVALID;
	location.instruction_id = ZEND_MIR_ID_INVALID;
	location.frame_state_id = ZEND_MIR_ID_INVALID;
	location.source_position_id = ZEND_MIR_ID_INVALID;
	return location;
}

zend_mir_diagnostic_location zend_mir_verify_function_location(
		const zend_mir_verify_context *context, zend_mir_function_id function_id)
{
	zend_mir_diagnostic_location location = zend_mir_verify_location();

	location.module_id = context != NULL ? context->module_id : ZEND_MIR_ID_INVALID;
	location.function_id = function_id;
	return location;
}

zend_mir_diagnostic_location zend_mir_verify_block_location(
		const zend_mir_verify_context *context, zend_mir_block_id block_id)
{
	const zend_mir_verify_block *block = zend_mir_verify_find_block(context, block_id);
	zend_mir_diagnostic_location location = zend_mir_verify_location();

	location.module_id = context != NULL ? context->module_id : ZEND_MIR_ID_INVALID;
	location.block_id = block_id;
	if (block != NULL) {
		location.function_id = block->record.function_id;
	}
	return location;
}

zend_mir_diagnostic_location zend_mir_verify_instruction_location(
		const zend_mir_verify_context *context, const zend_mir_instruction_record *instruction)
{
	zend_mir_diagnostic_location location;

	if (instruction == NULL) {
		location = zend_mir_verify_location();
		location.module_id = context != NULL ? context->module_id : ZEND_MIR_ID_INVALID;
		return location;
	}
	location = zend_mir_verify_block_location(context, instruction->block_id);
	location.instruction_id = instruction->id;
	location.frame_state_id = instruction->frame_state_id;
	location.source_position_id = instruction->source_position_id;
	return location;
}

zend_mir_diagnostic_location zend_mir_verify_frame_location(
		const zend_mir_verify_context *context, const zend_mir_frame_state_ref *frame)
{
	zend_mir_diagnostic_location location = zend_mir_verify_location();

	location.module_id = context != NULL ? context->module_id : ZEND_MIR_ID_INVALID;
	if (frame != NULL) {
		location.function_id = frame->function_id;
		location.frame_state_id = frame->id;
	}
	return location;
}

#define ZEND_MIR_VERIFY_FIND_FUNCTION(name, type, field, count_field, id_field) \
	const type *name(const zend_mir_verify_context *context, uint32_t id) \
	{ \
		uint32_t left = 0; \
		uint32_t right = context != NULL ? context->count_field : 0; \
		while (left < right) { \
			uint32_t middle = left + (right - left) / 2; \
			uint32_t current = context->field[middle].record.id_field; \
			if (current < id) { \
				left = middle + 1; \
			} else if (current > id) { \
				right = middle; \
			} else { \
				return &context->field[middle]; \
			} \
		} \
		return NULL; \
	}

ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_function,
	zend_mir_verify_function, functions, function_count, id)
ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_block,
	zend_mir_verify_block, blocks, block_count, id)
ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_instruction,
	zend_mir_verify_instruction, instructions, instruction_count, id)
ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_value,
	zend_mir_verify_value, values, value_count, id)
ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_frame,
	zend_mir_verify_frame, frames, frame_count, id)
ZEND_MIR_VERIFY_FIND_FUNCTION(zend_mir_verify_find_source,
	zend_mir_verify_source, sources, source_count, id)

#undef ZEND_MIR_VERIFY_FIND_FUNCTION

bool zend_mir_verify_span_is_valid(zend_mir_span span, uint32_t count)
{
	return span.offset <= count && span.count <= count - span.offset;
}

bool zend_mir_verify_mask_has_unknown(uint64_t mask, uint32_t count)
{
	uint64_t known;

	if (count >= 64) {
		return false;
	}
	known = count == 0 ? 0 : (UINT64_C(1) << count) - 1;
	return (mask & ~known) != 0;
}

static int zend_mir_verify_compare_function(const void *left, const void *right)
{
	const zend_mir_verify_function *a = (const zend_mir_verify_function *) left;
	const zend_mir_verify_function *b = (const zend_mir_verify_function *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_block(const void *left, const void *right)
{
	const zend_mir_verify_block *a = (const zend_mir_verify_block *) left;
	const zend_mir_verify_block *b = (const zend_mir_verify_block *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_instruction(const void *left, const void *right)
{
	const zend_mir_verify_instruction *a = (const zend_mir_verify_instruction *) left;
	const zend_mir_verify_instruction *b = (const zend_mir_verify_instruction *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_value(const void *left, const void *right)
{
	const zend_mir_verify_value *a = (const zend_mir_verify_value *) left;
	const zend_mir_verify_value *b = (const zend_mir_verify_value *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_constant(const void *left, const void *right)
{
	const zend_mir_verify_constant *a = (const zend_mir_verify_constant *) left;
	const zend_mir_verify_constant *b = (const zend_mir_verify_constant *) right;

	return a->record.value_id < b->record.value_id ? -1
		: a->record.value_id != b->record.value_id;
}

static int zend_mir_verify_compare_frame(const void *left, const void *right)
{
	const zend_mir_verify_frame *a = (const zend_mir_verify_frame *) left;
	const zend_mir_verify_frame *b = (const zend_mir_verify_frame *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_source(const void *left, const void *right)
{
	const zend_mir_verify_source *a = (const zend_mir_verify_source *) left;
	const zend_mir_verify_source *b = (const zend_mir_verify_source *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static int zend_mir_verify_compare_source_map(const void *left, const void *right)
{
	const zend_mir_verify_source_map *a = (const zend_mir_verify_source_map *) left;
	const zend_mir_verify_source_map *b = (const zend_mir_verify_source_map *) right;

	return a->record.id < b->record.id ? -1 : a->record.id != b->record.id;
}

static bool zend_mir_verify_view_is_complete(const zend_mir_view *view)
{
	return view != NULL && view->module_id != NULL
		&& view->function_count != NULL && view->function_at != NULL
		&& view->block_count != NULL && view->block_at != NULL
		&& view->instruction_count != NULL && view->instruction_at != NULL
		&& view->value_count != NULL && view->value_at != NULL
		&& view->constant_count != NULL && view->constant_at != NULL
		&& view->frame_state_count != NULL && view->frame_state_at != NULL
		&& view->source_position_count != NULL && view->source_position_at != NULL
		&& view->frame_slot_count != NULL && view->frame_slot_at != NULL
		&& view->root_count != NULL && view->root_at != NULL
		&& view->cleanup_count != NULL && view->cleanup_at != NULL
		&& view->instruction_operand_count != NULL && view->instruction_operand_at != NULL
		&& view->successor_count != NULL && view->successor_at != NULL
		&& view->predecessor_count != NULL && view->predecessor_at != NULL
		&& view->source_map_count != NULL && view->source_map_at != NULL;
}

static bool zend_mir_verify_count(zend_mir_verify_context *context,
		uint32_t count, const char *name)
{
	if (count <= ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT) {
		return true;
	}
	zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CAPACITY_EXCEEDED,
		ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED, name);
	return false;
}

#define ZEND_MIR_VERIFY_LOAD_ARRAY(context, count_field, pointer_field, type, callback, label) \
	do { \
		uint32_t load_index; \
		if ((context)->count_field != 0) { \
			(context)->pointer_field = zend_mir_verify_allocate( \
				(context), (context)->count_field, sizeof(type)); \
			if ((context)->pointer_field == NULL) { \
				return false; \
			} \
		} \
		for (load_index = 0; load_index < (context)->count_field; load_index++) { \
			if (!(context)->view->callback((context)->view->context, load_index, \
					&(context)->pointer_field[load_index].record)) { \
				zend_mir_verify_emit_fatal((context), ZEND_MIR_VERIFY_CALLBACK_FAILED, \
					ZEND_MIR_DIAGNOSTIC_INVALID_ID, label); \
				return false; \
			} \
		} \
	} while (0)

static bool zend_mir_verify_load_entities(zend_mir_verify_context *context)
{
	uint32_t index;

	context->function_count = context->view->function_count(context->view->context);
	context->block_count = context->view->block_count(context->view->context);
	context->instruction_count = context->view->instruction_count(context->view->context);
	context->value_count = context->view->value_count(context->view->context);
	context->constant_count = context->view->constant_count(context->view->context);
	context->frame_count = context->view->frame_state_count(context->view->context);
	context->source_count = context->view->source_position_count(context->view->context);
	context->source_map_count = context->view->source_map_count(context->view->context);
	context->slot_count = context->view->frame_slot_count(context->view->context);
	context->root_count = context->view->root_count(context->view->context);
	context->cleanup_count = context->view->cleanup_count(context->view->context);

	if (!zend_mir_verify_count(context, context->function_count, "function count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->block_count, "block count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->instruction_count,
				"instruction count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->value_count, "value count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->constant_count,
				"constant count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->frame_count,
				"frame-state count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->source_count,
				"source-position count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->source_map_count,
				"source-map count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->slot_count,
				"frame-slot count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->root_count, "root count exceeds hard limit")
			|| !zend_mir_verify_count(context, context->cleanup_count,
				"cleanup count exceeds hard limit")) {
		return false;
	}

	ZEND_MIR_VERIFY_LOAD_ARRAY(context, function_count, functions,
		zend_mir_verify_function, function_at, "function_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, block_count, blocks,
		zend_mir_verify_block, block_at, "block_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, instruction_count, instructions,
		zend_mir_verify_instruction, instruction_at, "instruction_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, value_count, values,
		zend_mir_verify_value, value_at, "value_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, constant_count, constants,
		zend_mir_verify_constant, constant_at, "constant_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, frame_count, frames,
		zend_mir_verify_frame, frame_state_at, "frame_state_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, source_count, sources,
		zend_mir_verify_source, source_position_at, "source_position_at callback failed");
	ZEND_MIR_VERIFY_LOAD_ARRAY(context, source_map_count, source_maps,
		zend_mir_verify_source_map, source_map_at, "source_map_at callback failed");

	if (context->slot_count != 0) {
		context->slots = zend_mir_verify_allocate(
			context, context->slot_count, sizeof(*context->slots));
		if (context->slots == NULL) {
			return false;
		}
	}
	for (index = 0; index < context->slot_count; index++) {
		if (!context->view->frame_slot_at(
				context->view->context, index, &context->slots[index])) {
			zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, "frame_slot_at callback failed");
			return false;
		}
	}
	if (context->root_count != 0) {
		context->roots = zend_mir_verify_allocate(
			context, context->root_count, sizeof(*context->roots));
		if (context->roots == NULL) {
			return false;
		}
	}
	for (index = 0; index < context->root_count; index++) {
		if (!context->view->root_at(context->view->context, index, &context->roots[index])) {
			zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, "root_at callback failed");
			return false;
		}
	}
	if (context->cleanup_count != 0) {
		context->cleanups = zend_mir_verify_allocate(
			context, context->cleanup_count, sizeof(*context->cleanups));
		if (context->cleanups == NULL) {
			return false;
		}
	}
	for (index = 0; index < context->cleanup_count; index++) {
		if (!context->view->cleanup_at(
				context->view->context, index, &context->cleanups[index])) {
			zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, "cleanup_at callback failed");
			return false;
		}
	}

#define ZEND_MIR_VERIFY_SORT(field, count, compare) \
	do { \
		if ((count) > 1) { \
			qsort((field), (count), sizeof(*(field)), (compare)); \
		} \
	} while (0)
	ZEND_MIR_VERIFY_SORT(
		context->functions, context->function_count, zend_mir_verify_compare_function);
	ZEND_MIR_VERIFY_SORT(
		context->blocks, context->block_count, zend_mir_verify_compare_block);
	ZEND_MIR_VERIFY_SORT(context->instructions,
		context->instruction_count, zend_mir_verify_compare_instruction);
	ZEND_MIR_VERIFY_SORT(
		context->values, context->value_count, zend_mir_verify_compare_value);
	ZEND_MIR_VERIFY_SORT(
		context->constants, context->constant_count, zend_mir_verify_compare_constant);
	ZEND_MIR_VERIFY_SORT(
		context->frames, context->frame_count, zend_mir_verify_compare_frame);
	ZEND_MIR_VERIFY_SORT(
		context->sources, context->source_count, zend_mir_verify_compare_source);
	ZEND_MIR_VERIFY_SORT(context->source_maps,
		context->source_map_count, zend_mir_verify_compare_source_map);
#undef ZEND_MIR_VERIFY_SORT
	return true;
}

#undef ZEND_MIR_VERIFY_LOAD_ARRAY

static bool zend_mir_verify_checked_relation_add(
		zend_mir_verify_context *context, uint32_t *total, uint32_t count)
{
	if (count > ZEND_MIR_VERIFY_RELATION_HARD_LIMIT
			|| *total > ZEND_MIR_VERIFY_RELATION_HARD_LIMIT - count) {
		zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CAPACITY_EXCEEDED,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"operand or CFG relation count exceeds hard limit");
		return false;
	}
	*total += count;
	return true;
}

static bool zend_mir_verify_load_relations(zend_mir_verify_context *context)
{
	uint32_t index;

	for (index = 0; index < context->instruction_count; index++) {
		zend_mir_verify_instruction *instruction = &context->instructions[index];
		uint32_t count = context->view->instruction_operand_count(
			context->view->context, instruction->record.id);

		instruction->operands_offset = context->operand_count;
		instruction->operands_count = count;
		if (!zend_mir_verify_checked_relation_add(context, &context->operand_count, count)) {
			return false;
		}
	}
	for (index = 0; index < context->block_count; index++) {
		zend_mir_verify_block *block = &context->blocks[index];
		uint32_t successors = context->view->successor_count(
			context->view->context, block->record.id);
		uint32_t predecessors = context->view->predecessor_count(
			context->view->context, block->record.id);

		block->successors_offset = context->successor_count;
		block->successors_count = successors;
		block->predecessors_offset = context->predecessor_count;
		block->predecessors_count = predecessors;
		if (!zend_mir_verify_checked_relation_add(
				context, &context->successor_count, successors)
				|| !zend_mir_verify_checked_relation_add(
					context, &context->predecessor_count, predecessors)) {
			return false;
		}
	}
	if (context->operand_count != 0) {
		context->operands = zend_mir_verify_allocate(
			context, context->operand_count, sizeof(*context->operands));
		if (context->operands == NULL) {
			return false;
		}
	}
	if (context->successor_count != 0) {
		context->successors = zend_mir_verify_allocate(
			context, context->successor_count, sizeof(*context->successors));
		if (context->successors == NULL) {
			return false;
		}
	}
	if (context->predecessor_count != 0) {
		context->predecessors = zend_mir_verify_allocate(
			context, context->predecessor_count, sizeof(*context->predecessors));
		if (context->predecessors == NULL) {
			return false;
		}
	}
	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_verify_instruction *instruction = &context->instructions[index];
		uint32_t operand_index;

		for (operand_index = 0; operand_index < instruction->operands_count; operand_index++) {
			if (!context->view->instruction_operand_at(context->view->context,
					instruction->record.id, operand_index,
					&context->operands[instruction->operands_offset + operand_index])) {
				zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
					ZEND_MIR_DIAGNOSTIC_INVALID_ID,
					"instruction_operand_at callback failed");
				return false;
			}
		}
	}
	for (index = 0; index < context->block_count; index++) {
		const zend_mir_verify_block *block = &context->blocks[index];
		uint32_t edge_index;

		for (edge_index = 0; edge_index < block->successors_count; edge_index++) {
			if (!context->view->successor_at(context->view->context,
					block->record.id, edge_index,
					&context->successors[block->successors_offset + edge_index])) {
				zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG, "successor_at callback failed");
				return false;
			}
		}
		for (edge_index = 0; edge_index < block->predecessors_count; edge_index++) {
			if (!context->view->predecessor_at(context->view->context,
					block->record.id, edge_index,
					&context->predecessors[block->predecessors_offset + edge_index])) {
				zend_mir_verify_emit_fatal(context, ZEND_MIR_VERIFY_CALLBACK_FAILED,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG, "predecessor_at callback failed");
				return false;
			}
		}
	}
	return true;
}

static void zend_mir_verify_initialize_diagnostics(zend_mir_verify_context *context)
{
	uint32_t available;

	if (context->diagnostics == NULL || context->diagnostics->emit == NULL
			|| context->diagnostics->emitted >= context->diagnostics->limit) {
		context->diagnostic_capacity = 0;
		return;
	}
	available = context->diagnostics->limit - context->diagnostics->emitted;
	context->diagnostic_capacity = available < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT
		? available : ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT;
}

bool zend_mir_verify_stage1(
		const zend_mir_view *view, zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_verify_context context;

	memset(&context, 0, sizeof(context));
	context.view = view;
	context.diagnostics = diagnostics;
	context.module_id = ZEND_MIR_ID_INVALID;
	context.valid = true;
	context.identifiers_valid = true;
	zend_mir_verify_initialize_diagnostics(&context);

	if (view == NULL) {
		zend_mir_verify_emit_fatal(&context, ZEND_MIR_VERIFY_INVALID_ARGUMENT,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, "view is null");
		goto done;
	}
	if (!zend_mir_verify_view_is_complete(view)) {
		zend_mir_verify_emit_fatal(&context, ZEND_MIR_VERIFY_INCOMPLETE_VIEW,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, "view callback table is incomplete");
		goto done;
	}
	if (!zend_mir_contract_is_compatible(view->contract_version)) {
		zend_mir_verify_emit_fatal(&context, ZEND_MIR_VERIFY_UNSUPPORTED_VERSION,
			ZEND_MIR_DIAGNOSTIC_UNSUPPORTED_CONTRACT_VERSION,
			"view contract version is unsupported");
		goto done;
	}
	context.module_id = view->module_id(view->context);
	if (!zend_mir_id_is_valid(context.module_id)) {
		zend_mir_verify_emit_fatal(&context, ZEND_MIR_VERIFY_INVALID_ID,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, "module ID is invalid");
		goto done;
	}
	if (!zend_mir_verify_load_entities(&context)) {
		goto done;
	}
	zend_mir_verify_ids(&context);
	if (!context.identifiers_valid || context.halted) {
		goto done;
	}
	if (!zend_mir_verify_load_relations(&context)) {
		goto done;
	}
	zend_mir_verify_cfg(&context);
	if (!context.halted) {
		zend_mir_verify_dominance(&context);
	}
	if (!context.halted) {
		zend_mir_verify_semantics(&context);
	}
	if (!context.halted) {
		zend_mir_verify_frames(&context);
	}

done:
	zend_mir_verify_flush_diagnostics(&context);
	zend_mir_verify_release(&context);
#ifdef ZEND_MIR_VERIFY_TESTING
	zend_mir_verify_allocations_before_failure = UINT32_MAX;
#endif
	return context.valid && !context.halted;
}
