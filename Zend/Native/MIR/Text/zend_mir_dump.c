/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "zend_mir_dump.h"

#include <string.h>

#include "../Core/zend_mir_module_internal.h"

typedef struct _zend_mir_dump_context {
	const zend_mir_view *view;
	zend_mir_text_writer *writer;
	zend_mir_diagnostic_sink *diagnostics;
	bool writer_failed;
} zend_mir_dump_context;

typedef bool (*zend_mir_dump_id_at_fn)(
	const zend_mir_view *view, uint32_t index, uint32_t *id_out);
typedef bool (*zend_mir_dump_record_fn)(zend_mir_dump_context *dump, uint32_t index);

static void zend_mir_dump_diagnostic(zend_mir_dump_context *dump, const char *message)
{
	zend_mir_diagnostic diagnostic;
	size_t length;

	if (dump->diagnostics == NULL) {
		return;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = ZEND_MIR_DIAGNOSTIC_INVALID_TEXT;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	length = strlen(message);
	if (length >= sizeof(diagnostic.message)) {
		length = sizeof(diagnostic.message) - 1;
	}
	memcpy(diagnostic.message, message, length);
	diagnostic.message[length] = '\0';
	(void) zend_mir_diagnostic_sink_emit(dump->diagnostics, &diagnostic);
}

static bool zend_mir_dump_write(zend_mir_dump_context *dump, const char *bytes, size_t length)
{
	if (dump->writer_failed) {
		return false;
	}
	if (dump->writer == NULL || dump->writer->write == NULL
			|| !dump->writer->write(dump->writer->context, bytes, length)) {
		dump->writer_failed = true;
		zend_mir_dump_diagnostic(dump, "ZNMIR text writer rejected output");
		return false;
	}
	return true;
}

static bool zend_mir_dump_literal(zend_mir_dump_context *dump, const char *literal)
{
	return zend_mir_dump_write(dump, literal, strlen(literal));
}

static bool zend_mir_dump_u64(zend_mir_dump_context *dump, uint64_t value)
{
	char buffer[20];
	size_t position = sizeof(buffer);

	do {
		buffer[--position] = (char) ('0' + value % 10);
		value /= 10;
	} while (value != 0);
	return zend_mir_dump_write(dump, buffer + position, sizeof(buffer) - position);
}

static bool zend_mir_dump_u32(zend_mir_dump_context *dump, uint32_t value)
{
	return zend_mir_dump_u64(dump, value);
}

static bool zend_mir_dump_i64(zend_mir_dump_context *dump, int64_t value)
{
	uint64_t magnitude;

	if (value >= 0) {
		return zend_mir_dump_u64(dump, (uint64_t) value);
	}
	magnitude = (uint64_t) (-(value + 1));
	magnitude++;
	return zend_mir_dump_literal(dump, "-")
		&& zend_mir_dump_u64(dump, magnitude);
}

static bool zend_mir_dump_hex(zend_mir_dump_context *dump, uint64_t value, uint32_t digits)
{
	static const char hex[] = "0123456789abcdef";
	char buffer[16];
	uint32_t index;

	if (digits > sizeof(buffer)) {
		return false;
	}
	for (index = 0; index < digits; index++) {
		uint32_t shift = (digits - index - 1) * 4;
		buffer[index] = hex[(value >> shift) & UINT64_C(0xf)];
	}
	return zend_mir_dump_write(dump, buffer, digits);
}

static bool zend_mir_dump_bool(zend_mir_dump_context *dump, bool value)
{
	return zend_mir_dump_literal(dump, value ? "true" : "false");
}

static bool zend_mir_dump_id(zend_mir_dump_context *dump, const char *prefix, uint32_t id)
{
	if (id == ZEND_MIR_ID_INVALID) {
		return zend_mir_dump_literal(dump, "invalid");
	}
	return zend_mir_dump_literal(dump, prefix) && zend_mir_dump_u32(dump, id);
}

static bool zend_mir_dump_scalar_id(zend_mir_dump_context *dump, uint32_t id)
{
	return id == ZEND_MIR_ID_INVALID
		? zend_mir_dump_literal(dump, "invalid")
		: zend_mir_dump_u32(dump, id);
}

#define ZEND_MIR_LABEL_CASE(symbol, label, number) case number: return label;

static const char *zend_mir_opcode_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_SCALAR_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_CALL_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_VALUE_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_ITERATOR_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		ZEND_MIR_OBJECT_OPCODE_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_call_target_kind_label(uint32_t value)
{
	switch (value) {
		case ZEND_MIR_CALL_TARGET_DIRECT_USER:
			return "direct_user";
		case ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL:
			return "direct_internal";
		case ZEND_MIR_CALL_TARGET_METHOD_USER:
			return "method_user";
		case ZEND_MIR_CALL_TARGET_DYNAMIC:
			return "dynamic";
		default:
			return NULL;
	}
}

static const char *zend_mir_call_argument_ownership_label(uint32_t value)
{
	return value == ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
		? "borrowed_scalar" : NULL;
}

static const char *zend_mir_call_continuation_kind_label(uint32_t value)
{
	switch (value) {
		case ZEND_MIR_CALL_CONTINUATION_NORMAL:
			return "normal";
		case ZEND_MIR_CALL_CONTINUATION_EXCEPTION_DEBT:
			return "exception_debt";
		case ZEND_MIR_CALL_CONTINUATION_BAILOUT_REENTRY_DEBT:
			return "bailout_reentry_debt";
		case ZEND_MIR_CALL_CONTINUATION_OBSERVER_DEBT:
			return "observer_debt";
		default:
			return NULL;
	}
}

static const char *zend_mir_representation_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_REPRESENTATION_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_constant_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_CONSTANT_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_ownership_state_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_OWNERSHIP_STATE_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_function_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_FUNCTION_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_opline_phase_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_OPLINE_PHASE_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_slot_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_FRAME_SLOT_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_slot_representation_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_FRAME_SLOT_REPRESENTATION_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_materialization_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_MATERIALIZATION_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_slot_ownership_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_FRAME_SLOT_OWNERSHIP_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_cleanup_action_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_CLEANUP_ACTION_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_cleanup_state_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_CLEANUP_STATE_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_continuation_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_CONTINUATION_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_suspend_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_SUSPEND_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_resume_entry_kind_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_RESUME_ENTRY_KIND_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_safepoint_class_label(uint32_t value)
{
	switch (value) {
		ZEND_MIR_SAFEPOINT_CLASS_CATALOG(ZEND_MIR_LABEL_CASE)
		default: return NULL;
	}
}

static const char *zend_mir_scalar_type_label(uint32_t value)
{
	switch (value) {
		case ZEND_MIR_SCALAR_TYPE_NULL: return "null";
		case ZEND_MIR_SCALAR_TYPE_I1: return "i1";
		case ZEND_MIR_SCALAR_TYPE_I64: return "i64";
		case ZEND_MIR_SCALAR_TYPE_F64: return "f64";
		default: return NULL;
	}
}

static const char *zend_mir_fact_provenance_label(uint32_t value)
{
	switch (value) {
		case ZEND_MIR_FACT_PROVENANCE_SSA: return "ssa";
		case ZEND_MIR_FACT_PROVENANCE_LITERAL: return "literal";
		case ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS: return "range_analysis";
		case ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS: return "type_analysis";
		case ZEND_MIR_FACT_PROVENANCE_CONTRACT: return "contract";
		default: return NULL;
	}
}

#undef ZEND_MIR_LABEL_CASE

static bool zend_mir_dump_label(
		zend_mir_dump_context *dump, const char *(*label_fn)(uint32_t), uint32_t value)
{
	const char *label = label_fn(value);

	if (label != NULL) {
		return zend_mir_dump_literal(dump, label);
	}
	return zend_mir_dump_literal(dump, "invalid(")
		&& zend_mir_dump_u32(dump, value)
		&& zend_mir_dump_literal(dump, ")");
}

static bool zend_mir_dump_block_list(zend_mir_dump_context *dump, uint32_t count,
		bool (*at)(const void *, zend_mir_block_id, uint32_t, zend_mir_block_id *),
		zend_mir_block_id block_id)
{
	uint32_t index;

	if (!zend_mir_dump_literal(dump, "[")) {
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_block_id id;
		if (index != 0 && !zend_mir_dump_literal(dump, ", ")) {
			return false;
		}
		if (!at(dump->view->context, block_id, index, &id)) {
			if (!zend_mir_dump_literal(dump, "invalid")) {
				return false;
			}
		} else if (!zend_mir_dump_id(dump, "b", id)) {
			return false;
		}
	}
	return zend_mir_dump_literal(dump, "]");
}

static bool zend_mir_dump_operand_list(
		zend_mir_dump_context *dump, zend_mir_instruction_id instruction_id)
{
	uint32_t count = dump->view->instruction_operand_count(
		dump->view->context, instruction_id);
	uint32_t index;

	if (!zend_mir_dump_literal(dump, "[")) {
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_value_id id;
		if (index != 0 && !zend_mir_dump_literal(dump, ", ")) {
			return false;
		}
		if (!dump->view->instruction_operand_at(
				dump->view->context, instruction_id, index, &id)) {
			if (!zend_mir_dump_literal(dump, "invalid")) {
				return false;
			}
		} else if (!zend_mir_dump_id(dump, "v", id)) {
			return false;
		}
	}
	return zend_mir_dump_literal(dump, "]");
}

static bool zend_mir_dump_continuation(
		zend_mir_dump_context *dump, const zend_mir_continuation_ref *continuation)
{
	return zend_mir_dump_label(dump, zend_mir_continuation_kind_label, continuation->kind)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_id(dump, "fs", continuation->frame_state_id)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_scalar_id(dump, continuation->opline_index);
}

static bool zend_mir_function_id_at(
		const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_function_record record;
	if (!view->function_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_block_id_at(const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_block_record record;
	if (!view->block_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_value_id_at(const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_value_record record;
	if (!view->value_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_constant_id_at(const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_constant_record record;
	if (!view->constant_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.value_id;
	return true;
}

static bool zend_mir_value_fact_id_at(
		const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_value_fact_ref record;

	if (view->value_fact_at == NULL
			|| !view->value_fact_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.value_id;
	return true;
}

static bool zend_mir_source_id_at(const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_source_position_ref record;
	if (!view->source_position_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_frame_id_at(const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_frame_state_ref record;
	if (!view->frame_state_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_instruction_id_at(
		const zend_mir_view *view, uint32_t index, uint32_t *id_out)
{
	zend_mir_instruction_record record;
	if (!view->instruction_at(view->context, index, &record)) {
		return false;
	}
	*id_out = record.id;
	return true;
}

static bool zend_mir_dump_function(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_function_record record;

	if (!dump->view->function_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "function ")
		&& zend_mir_dump_id(dump, "f", record.id)
		&& zend_mir_dump_literal(dump, " symbol ")
		&& zend_mir_dump_id(dump, "s", record.symbol_id)
		&& zend_mir_dump_literal(dump, " entry ")
		&& zend_mir_dump_id(dump, "b", record.entry_block_id)
		&& zend_mir_dump_literal(dump, " flags 0x")
		&& zend_mir_dump_hex(dump, record.flags, 8)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_block(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_block_record record;
	uint32_t predecessor_count;
	uint32_t successor_count;

	if (!dump->view->block_at(dump->view->context, index, &record)) {
		return false;
	}
	predecessor_count = dump->view->predecessor_count(dump->view->context, record.id);
	successor_count = dump->view->successor_count(dump->view->context, record.id);
	return zend_mir_dump_literal(dump, "block ")
		&& zend_mir_dump_id(dump, "b", record.id)
		&& zend_mir_dump_literal(dump, " function ")
		&& zend_mir_dump_id(dump, "f", record.function_id)
		&& zend_mir_dump_literal(dump, " predecessors ")
		&& zend_mir_dump_block_list(dump, predecessor_count,
			dump->view->predecessor_at, record.id)
		&& zend_mir_dump_literal(dump, " successors ")
		&& zend_mir_dump_block_list(dump, successor_count,
			dump->view->successor_at, record.id)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_value(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_value_record record;

	if (!dump->view->value_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "value ")
		&& zend_mir_dump_id(dump, "v", record.id)
		&& zend_mir_dump_literal(dump, " representation ")
		&& zend_mir_dump_label(dump, zend_mir_representation_label, record.representation)
		&& zend_mir_dump_literal(dump, " ownership ")
		&& zend_mir_dump_label(dump, zend_mir_ownership_state_label, record.ownership)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_constant(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_constant_record record;

	if (!dump->view->constant_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "constant ")
		&& zend_mir_dump_id(dump, "v", record.value_id)
		&& zend_mir_dump_literal(dump, " representation ")
		&& zend_mir_dump_label(dump, zend_mir_representation_label, record.representation)
		&& zend_mir_dump_literal(dump, " kind ")
		&& zend_mir_dump_label(dump, zend_mir_constant_kind_label, record.kind)
		&& zend_mir_dump_literal(dump, " payload 0x")
		&& zend_mir_dump_hex(dump, record.payload_bits, 16)
		&& zend_mir_dump_literal(dump, " symbol ")
		&& zend_mir_dump_id(dump, "s", record.symbol_id)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_value_fact(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_value_fact_ref record;

	if (dump->view->value_fact_at == NULL
			|| !dump->view->value_fact_at(dump->view->context, index, &record)) {
		return false;
	}
	if (!zend_mir_dump_literal(dump, "fact ")
			|| !zend_mir_dump_id(dump, "vf", record.id)
			|| !zend_mir_dump_literal(dump, " value ")
			|| !zend_mir_dump_id(dump, "v", record.value_id)
			|| !zend_mir_dump_literal(dump, " type ")
			|| !zend_mir_dump_label(dump, zend_mir_scalar_type_label,
				record.exact_type)
			|| !zend_mir_dump_literal(dump, " flags 0x")
			|| !zend_mir_dump_hex(dump, record.flags, 8)
			|| !zend_mir_dump_literal(dump, " range ")) {
		return false;
	}
	if ((record.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0) {
		if (!zend_mir_dump_i64(dump, record.integer_min)
				|| !zend_mir_dump_literal(dump, ":")
				|| !zend_mir_dump_i64(dump, record.integer_max)) {
			return false;
		}
	} else if (!zend_mir_dump_literal(dump, "none")) {
		return false;
	}
	return zend_mir_dump_literal(dump, " provenance ")
		&& zend_mir_dump_label(dump, zend_mir_fact_provenance_label,
			record.provenance)
		&& zend_mir_dump_literal(dump, " source ")
		&& zend_mir_dump_id(dump, "p",
			record.provenance_source_position_id)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_source(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_source_position_ref record;

	if (!dump->view->source_position_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "source ")
		&& zend_mir_dump_id(dump, "p", record.id)
		&& zend_mir_dump_literal(dump, " file ")
		&& zend_mir_dump_id(dump, "s", record.file_symbol_id)
		&& zend_mir_dump_literal(dump, " line ")
		&& zend_mir_dump_u32(dump, record.line)
		&& zend_mir_dump_literal(dump, " columns ")
		&& zend_mir_dump_u32(dump, record.column_start)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_u32(dump, record.column_end)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_slot(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_frame_slot_ref record;

	if (!dump->view->frame_slot_at(dump->view->context, index, &record)) {
		return zend_mir_dump_literal(dump, "invalid slot index ")
			&& zend_mir_dump_u32(dump, index)
			&& zend_mir_dump_literal(dump, "\n");
	}
	return zend_mir_dump_literal(dump, "slot ")
		&& zend_mir_dump_u32(dump, record.slot_id)
		&& zend_mir_dump_literal(dump, " value ")
		&& zend_mir_dump_id(dump, "v", record.value_id)
		&& zend_mir_dump_literal(dump, " index ")
		&& zend_mir_dump_scalar_id(dump, record.index)
		&& zend_mir_dump_literal(dump, " kind ")
		&& zend_mir_dump_label(dump, zend_mir_slot_kind_label, record.kind)
		&& zend_mir_dump_literal(dump, " representation ")
		&& zend_mir_dump_label(dump, zend_mir_slot_representation_label, record.representation)
		&& zend_mir_dump_literal(dump, " materialization ")
		&& zend_mir_dump_label(dump, zend_mir_materialization_label, record.materialization)
		&& zend_mir_dump_literal(dump, " ownership ")
		&& zend_mir_dump_label(dump, zend_mir_slot_ownership_label, record.ownership)
		&& zend_mir_dump_literal(dump, " rooted ")
		&& zend_mir_dump_bool(dump, record.rooted)
		&& zend_mir_dump_literal(dump, " cleanup ")
		&& zend_mir_dump_bool(dump, record.cleanup_required)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_root(zend_mir_dump_context *dump, uint32_t index)
{
	uint32_t slot_id;

	if (!dump->view->root_at(dump->view->context, index, &slot_id)) {
		return zend_mir_dump_literal(dump, "invalid root index ")
			&& zend_mir_dump_u32(dump, index)
			&& zend_mir_dump_literal(dump, "\n");
	}
	return zend_mir_dump_literal(dump, "root ")
		&& zend_mir_dump_u32(dump, slot_id)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_cleanup(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_cleanup_ref record;

	if (!dump->view->cleanup_at(dump->view->context, index, &record)) {
		return zend_mir_dump_literal(dump, "invalid cleanup index ")
			&& zend_mir_dump_u32(dump, index)
			&& zend_mir_dump_literal(dump, "\n");
	}
	return zend_mir_dump_literal(dump, "cleanup ")
		&& zend_mir_dump_u32(dump, record.slot_id)
		&& zend_mir_dump_literal(dump, " action ")
		&& zend_mir_dump_label(dump, zend_mir_cleanup_action_label, record.action)
		&& zend_mir_dump_literal(dump, " state ")
		&& zend_mir_dump_label(dump, zend_mir_cleanup_state_label, record.state)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_frame(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_frame_state_ref record;

	if (!dump->view->frame_state_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "frame ")
		&& zend_mir_dump_id(dump, "fs", record.id)
		&& zend_mir_dump_literal(dump, " function ")
		&& zend_mir_dump_id(dump, "f", record.function_id)
		&& zend_mir_dump_literal(dump, " parent ")
		&& zend_mir_dump_id(dump, "fs", record.parent_id)
		&& zend_mir_dump_literal(dump, " function-kind ")
		&& zend_mir_dump_label(dump, zend_mir_function_kind_label, record.function_kind)
		&& zend_mir_dump_literal(dump, " opline ")
		&& zend_mir_dump_scalar_id(dump, record.opline_index)
		&& zend_mir_dump_literal(dump, " phase ")
		&& zend_mir_dump_label(dump, zend_mir_opline_phase_label, record.opline_phase)
		&& zend_mir_dump_literal(dump, " slots ")
		&& zend_mir_dump_u32(dump, record.slots.offset)
		&& zend_mir_dump_literal(dump, "+")
		&& zend_mir_dump_u32(dump, record.slots.count)
		&& zend_mir_dump_literal(dump, " roots ")
		&& zend_mir_dump_u32(dump, record.roots.offset)
		&& zend_mir_dump_literal(dump, "+")
		&& zend_mir_dump_u32(dump, record.roots.count)
		&& zend_mir_dump_literal(dump, " cleanups ")
		&& zend_mir_dump_u32(dump, record.cleanup_obligations.offset)
		&& zend_mir_dump_literal(dump, "+")
		&& zend_mir_dump_u32(dump, record.cleanup_obligations.count)
		&& zend_mir_dump_literal(dump, " return ")
		&& zend_mir_dump_continuation(dump, &record.return_continuation)
		&& zend_mir_dump_literal(dump, " exception ")
		&& zend_mir_dump_continuation(dump, &record.exception_continuation)
		&& zend_mir_dump_literal(dump, " bailout ")
		&& zend_mir_dump_continuation(dump, &record.bailout_continuation)
		&& zend_mir_dump_literal(dump, " suspend ")
		&& zend_mir_dump_label(dump, zend_mir_suspend_kind_label, record.suspend_kind)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_scalar_id(dump, record.suspend_state_id)
		&& zend_mir_dump_literal(dump, " code-version ")
		&& zend_mir_dump_scalar_id(dump, record.code_version_id)
		&& zend_mir_dump_literal(dump, " resume ")
		&& zend_mir_dump_bool(dump, record.resume.allowed)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_label(dump, zend_mir_resume_entry_kind_label, record.resume.entry_kind)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_id(dump, "r", record.resume.resume_id)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_scalar_id(dump, record.resume.code_version_id)
		&& zend_mir_dump_literal(dump, ":")
		&& zend_mir_dump_scalar_id(dump, record.resume.target_opline_index)
		&& zend_mir_dump_literal(dump, " safepoint ")
		&& zend_mir_dump_label(dump, zend_mir_safepoint_class_label, record.safepoint_class)
		&& zend_mir_dump_literal(dump, " canonical ")
		&& zend_mir_dump_bool(dump, record.canonical)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_instruction(zend_mir_dump_context *dump, uint32_t index)
{
	zend_mir_instruction_record record;

	if (!dump->view->instruction_at(dump->view->context, index, &record)) {
		return false;
	}
	return zend_mir_dump_literal(dump, "instruction ")
		&& zend_mir_dump_id(dump, "i", record.id)
		&& zend_mir_dump_literal(dump, " block ")
		&& zend_mir_dump_id(dump, "b", record.block_id)
		&& zend_mir_dump_literal(dump, " opcode ")
		&& zend_mir_dump_label(dump, zend_mir_opcode_label, record.opcode)
		&& zend_mir_dump_literal(dump, " representation ")
		&& zend_mir_dump_label(dump, zend_mir_representation_label, record.representation)
		&& zend_mir_dump_literal(dump, " result ")
		&& zend_mir_dump_id(dump, "v", record.result_id)
		&& zend_mir_dump_literal(dump, " operands ")
		&& zend_mir_dump_operand_list(dump, record.id)
		&& zend_mir_dump_literal(dump, " effects 0x")
		&& zend_mir_dump_hex(dump, record.effects, 4)
		&& zend_mir_dump_literal(dump, " reads 0x")
		&& zend_mir_dump_hex(dump, record.reads, 8)
		&& zend_mir_dump_literal(dump, " writes 0x")
		&& zend_mir_dump_hex(dump, record.writes, 8)
		&& zend_mir_dump_literal(dump, " barriers 0x")
		&& zend_mir_dump_hex(dump, record.barriers, 2)
		&& zend_mir_dump_literal(dump, " ownership-actions 0x")
		&& zend_mir_dump_hex(dump, record.ownership_actions, 4)
		&& zend_mir_dump_literal(dump, " frame ")
		&& zend_mir_dump_id(dump, "fs", record.frame_state_id)
		&& zend_mir_dump_literal(dump, " source ")
		&& zend_mir_dump_id(dump, "p", record.source_position_id)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_call_frame(zend_mir_dump_context *dump,
	const char *label, const zend_mir_call_frame_descriptor *frame)
{
	return zend_mir_dump_literal(dump, label)
		&& zend_mir_dump_literal(dump, " frame ")
		&& zend_mir_dump_id(dump, "fs", frame->frame_state_id)
		&& zend_mir_dump_literal(dump, " function ")
		&& zend_mir_dump_id(dump, "f", frame->function_id)
		&& zend_mir_dump_literal(dump, " symbol ")
		&& zend_mir_dump_id(dump, "s", frame->function_symbol_id)
		&& zend_mir_dump_literal(dump, " op-array ")
		&& zend_mir_dump_id(dump, "oa", frame->op_array_id)
		&& zend_mir_dump_literal(dump, " slots ")
		&& zend_mir_dump_u32(dump, frame->slots.offset)
		&& zend_mir_dump_literal(dump, "+")
		&& zend_mir_dump_u32(dump, frame->slots.count)
		&& zend_mir_dump_literal(dump, " pending ")
		&& zend_mir_dump_scalar_id(dump, frame->pending_call_slot_id);
}

static bool zend_mir_dump_call_model(zend_mir_dump_context *dump)
{
	const zend_mir_call_view *calls =
		zend_mir_module_call_view_from_view(dump->view);
	uint32_t count;
	uint32_t index;

	if (calls == NULL) {
		return true;
	}
	if (calls->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| calls->call_target_count == NULL
			|| calls->call_target_at == NULL
			|| calls->call_argument_count == NULL
			|| calls->call_argument_at == NULL
			|| calls->call_continuation_count == NULL
			|| calls->call_continuation_at == NULL
			|| calls->call_site_count == NULL
			|| calls->call_site_at == NULL) {
		zend_mir_dump_diagnostic(dump, "incomplete W05 call view");
		return false;
	}
	count = calls->call_target_count(calls->context);
	if (count > UINT32_C(1048576)) {
		zend_mir_dump_diagnostic(dump, "W05 call-target count exceeds hard limit");
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_call_target_ref target;
		if (!calls->call_target_at(calls->context, index, &target)
				|| !zend_mir_dump_literal(dump, "call-target ")
				|| !zend_mir_dump_id(dump, "ct", target.id)
				|| !zend_mir_dump_literal(dump, " kind ")
				|| !zend_mir_dump_label(
					dump, zend_mir_call_target_kind_label, target.kind)
				|| !zend_mir_dump_literal(dump, " symbol ")
				|| !zend_mir_dump_id(
					dump, "s", target.function_symbol_id)
				|| !zend_mir_dump_literal(dump, " op-array ")
				|| !zend_mir_dump_id(dump, "oa", target.op_array_id)
				|| !zend_mir_dump_literal(dump, " args ")
				|| !zend_mir_dump_u32(dump, target.num_args)
				|| !zend_mir_dump_literal(dump, " required ")
				|| !zend_mir_dump_u32(dump, target.required_num_args)
				|| !zend_mir_dump_literal(dump, " flags 0x")
				|| !zend_mir_dump_hex(
					dump, target.function_flags_snapshot, 8)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	count = calls->call_argument_count(calls->context);
	if (count > UINT32_C(1048576)) {
		zend_mir_dump_diagnostic(dump, "W05 call-argument count exceeds hard limit");
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_call_argument_ref argument;
		if (!calls->call_argument_at(calls->context, index, &argument)
				|| !zend_mir_dump_literal(dump, "call-argument ")
				|| !zend_mir_dump_id(dump, "ca", argument.id)
				|| !zend_mir_dump_literal(dump, " site ")
				|| !zend_mir_dump_id(dump, "cs", argument.call_site_id)
				|| !zend_mir_dump_literal(dump, " ordinal ")
				|| !zend_mir_dump_u32(dump, argument.ordinal)
				|| !zend_mir_dump_literal(dump, " value ")
				|| !zend_mir_dump_id(dump, "v", argument.value_id)
				|| !zend_mir_dump_literal(dump, " ownership ")
				|| !zend_mir_dump_label(
					dump, zend_mir_call_argument_ownership_label,
					argument.ownership)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	count = calls->call_continuation_count(calls->context);
	if (count > UINT32_C(1048576)) {
		zend_mir_dump_diagnostic(
			dump, "W05 call-continuation count exceeds hard limit");
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_call_continuation_ref continuation;
		if (!calls->call_continuation_at(
				calls->context, index, &continuation)
				|| !zend_mir_dump_literal(dump, "call-continuation ")
				|| !zend_mir_dump_id(dump, "cc", continuation.id)
				|| !zend_mir_dump_literal(dump, " site ")
				|| !zend_mir_dump_id(
					dump, "cs", continuation.call_site_id)
				|| !zend_mir_dump_literal(dump, " kind ")
				|| !zend_mir_dump_label(
					dump, zend_mir_call_continuation_kind_label,
					continuation.kind)
				|| !zend_mir_dump_literal(dump, " block ")
				|| !zend_mir_dump_id(dump, "b", continuation.block_id)
				|| !zend_mir_dump_literal(dump, " debt 0x")
				|| !zend_mir_dump_hex(
					dump, continuation.semantic_debt, 8)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	count = calls->call_site_count(calls->context);
	if (count > UINT32_C(1048576)) {
		zend_mir_dump_diagnostic(dump, "W05 call-site count exceeds hard limit");
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_call_site_ref site;
		if (!calls->call_site_at(calls->context, index, &site)
				|| !zend_mir_dump_literal(dump, "call-site ")
				|| !zend_mir_dump_id(dump, "cs", site.id)
				|| !zend_mir_dump_literal(dump, " source ")
				|| !zend_mir_dump_id(
					dump, "sc", site.source_call_site_id)
				|| !zend_mir_dump_literal(dump, " instruction ")
				|| !zend_mir_dump_id(dump, "i", site.instruction_id)
				|| !zend_mir_dump_literal(dump, " target ")
				|| !zend_mir_dump_id(dump, "ct", site.target_id)
				|| !zend_mir_dump_literal(dump, " arguments ")
				|| !zend_mir_dump_u32(dump, site.arguments.offset)
				|| !zend_mir_dump_literal(dump, "+")
				|| !zend_mir_dump_u32(dump, site.arguments.count)
				|| !zend_mir_dump_literal(dump, " result ")
				|| !zend_mir_dump_id(dump, "v", site.result_id)
				|| !zend_mir_dump_literal(dump, " result-operand ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) site.result_operand.kind)
				|| !zend_mir_dump_literal(dump, ":")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) site.result_operand.slot_kind)
				|| !zend_mir_dump_literal(dump, ":")
				|| !zend_mir_dump_u32(dump, site.result_operand.index)
				|| !zend_mir_dump_literal(dump, ":")
				|| !zend_mir_dump_u32(
					dump, site.result_operand.ssa_variable_id)
				|| !zend_mir_dump_call_frame(
					dump, " caller", &site.caller_frame)
				|| !zend_mir_dump_call_frame(
					dump, " callee", &site.callee_entry_frame)
				|| !zend_mir_dump_literal(dump, " continuations ")
				|| !zend_mir_dump_u32(dump, site.continuations.offset)
				|| !zend_mir_dump_literal(dump, "+")
				|| !zend_mir_dump_u32(dump, site.continuations.count)
				|| !zend_mir_dump_literal(dump, " effects 0x")
				|| !zend_mir_dump_hex(dump, site.effects, 4)
				|| !zend_mir_dump_literal(dump, " reads 0x")
				|| !zend_mir_dump_hex(dump, site.reads, 8)
				|| !zend_mir_dump_literal(dump, " writes 0x")
				|| !zend_mir_dump_hex(dump, site.writes, 8)
				|| !zend_mir_dump_literal(dump, " barriers 0x")
				|| !zend_mir_dump_hex(dump, site.barriers, 2)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_dump_value_model(zend_mir_dump_context *dump)
{
	const zend_mir_value_view *values =
		zend_mir_module_value_view_from_view(dump->view);
	const zend_mir_module *module;
	uint32_t count;
	uint32_t index;

	if (values == NULL) {
		return true;
	}
	module = zend_mir_module_from_value_view(values);
	if (module == NULL
			|| (values->contract_version != ZEND_MIR_W06_CONTRACT_VERSION
				&& values->contract_version
					!= ZEND_MIR_W11P_CONTRACT_VERSION)
			|| values->storage_count == NULL || values->storage_at == NULL
			|| values->payload_count == NULL || values->payload_at == NULL
			|| values->reference_cell_count == NULL
			|| values->reference_cell_at == NULL
			|| values->alias_relation_count == NULL
			|| values->alias_relation_at == NULL
			|| values->ownership_event_count == NULL
			|| values->ownership_event_at == NULL
			|| values->separation_plan_count == NULL
			|| values->separation_plan_at == NULL
			|| values->call_transfer_count == NULL
			|| values->call_transfer_at == NULL
			|| (values->contract_version == ZEND_MIR_W11P_CONTRACT_VERSION
				&& ((values->model_flags
						& ~ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) != 0
					|| values->value_location_count == NULL
					|| values->value_location_at == NULL))) {
		zend_mir_dump_diagnostic(dump, "incomplete W06 value view");
		return false;
	}
#define ZEND_MIR_VALUE_COUNT(field) \
	count = values->field##_count(values->context); \
	if (count > UINT32_C(1048576)) { \
		zend_mir_dump_diagnostic(dump, \
			"W06 value table count exceeds hard limit"); \
		return false; \
	}
	ZEND_MIR_VALUE_COUNT(payload)
	for (index = 0; index < count; index++) {
		zend_mir_payload_ref record;
		if (!values->payload_at(values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "value-payload ")
				|| !zend_mir_dump_id(dump, "vp", record.id)
				|| !zend_mir_dump_literal(dump, " category ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.category)
				|| !zend_mir_dump_literal(dump, " refcount ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.refcount_state)
				|| !zend_mir_dump_literal(dump, " cleanup ")
				|| !zend_mir_dump_bool(
					dump, record.cleanup_obligation)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	ZEND_MIR_VALUE_COUNT(storage)
	for (index = 0; index < count; index++) {
		zend_mir_storage_ref record;
		if (!values->storage_at(values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "value-storage ")
				|| !zend_mir_dump_id(dump, "vs", record.id)
				|| !zend_mir_dump_literal(dump, " kind ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.kind)
				|| !zend_mir_dump_literal(dump, " state ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.state)
				|| !zend_mir_dump_literal(dump, " category ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.category)
				|| !zend_mir_dump_literal(dump, " payload ")
				|| !zend_mir_dump_id(dump, "vp", record.payload_id)
				|| !zend_mir_dump_literal(dump, " reference ")
				|| !zend_mir_dump_id(
					dump, "rc", record.reference_cell_id)
				|| !zend_mir_dump_literal(dump, " indirect ")
				|| !zend_mir_dump_id(
					dump, "vs", record.indirect_target_id)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	if (values->contract_version == ZEND_MIR_W11P_CONTRACT_VERSION) {
		if (!zend_mir_dump_literal(dump, "value-model-flags ")
				|| !zend_mir_dump_u32(dump, values->model_flags)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
		ZEND_MIR_VALUE_COUNT(value_location)
		for (index = 0; index < count; index++) {
			zend_mir_value_location_ref record;
			if (!values->value_location_at(
					values->context, index, &record)
					|| !zend_mir_dump_literal(
						dump, "value-location ")
					|| !zend_mir_dump_id(dump, "v", record.value_id)
					|| !zend_mir_dump_literal(
						dump, " frame-storage ")
					|| !zend_mir_dump_u32(dump, record.storage_id)
					|| !zend_mir_dump_literal(dump, "\n")) {
				return false;
			}
		}
	}
	ZEND_MIR_VALUE_COUNT(reference_cell)
	for (index = 0; index < count; index++) {
		zend_mir_reference_cell_ref record;
		if (!values->reference_cell_at(values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "reference-cell ")
				|| !zend_mir_dump_id(dump, "rc", record.id)
				|| !zend_mir_dump_literal(dump, " storage ")
				|| !zend_mir_dump_id(
					dump, "vs", record.payload_storage_id)
				|| !zend_mir_dump_literal(dump, " alias ")
				|| !zend_mir_dump_id(
					dump, "ac", record.alias_class_id)
				|| !zend_mir_dump_literal(dump, " source ")
				|| !zend_mir_dump_id(
					dump, "p", record.creation_source_id)
				|| !zend_mir_dump_literal(dump, " ownership ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.ownership)
				|| !zend_mir_dump_literal(dump, " cleanup ")
				|| !zend_mir_dump_bool(
					dump, record.cleanup_obligation)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	ZEND_MIR_VALUE_COUNT(alias_relation)
	for (index = 0; index < count; index++) {
		zend_mir_alias_relation_ref record;
		if (!values->alias_relation_at(values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "alias-relation ")
				|| !zend_mir_dump_id(dump, "ac", record.left_id)
				|| !zend_mir_dump_literal(dump, " ")
				|| !zend_mir_dump_id(dump, "ac", record.right_id)
				|| !zend_mir_dump_literal(dump, " relation ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.relation)
				|| !zend_mir_dump_literal(dump, " proof ")
				|| !zend_mir_dump_u32(dump, record.proof_id)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	ZEND_MIR_VALUE_COUNT(ownership_event)
	for (index = 0; index < count; index++) {
		zend_mir_ownership_event_ref record;
		if (!values->ownership_event_at(
				values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "ownership-event ")
				|| !zend_mir_dump_id(dump, "oe", record.id)
				|| !zend_mir_dump_literal(dump, " source ")
				|| !zend_mir_dump_id(
					dump, "vs", record.source_storage_id)
				|| !zend_mir_dump_literal(dump, " target ")
				|| !zend_mir_dump_id(
					dump, "vs", record.target_storage_id)
				|| !zend_mir_dump_literal(dump, " payload ")
				|| !zend_mir_dump_id(dump, "vp", record.payload_id)
				|| !zend_mir_dump_literal(dump, " action ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.action)
				|| !zend_mir_dump_literal(dump, " transition ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.before_state)
				|| !zend_mir_dump_literal(dump, "->")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.after_state)
				|| !zend_mir_dump_literal(dump, " cleanup ")
				|| !zend_mir_dump_bool(
					dump, record.cleanup_obligation)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	ZEND_MIR_VALUE_COUNT(separation_plan)
	for (index = 0; index < count; index++) {
		zend_mir_separation_plan_ref record;
		if (!values->separation_plan_at(
				values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "separation-plan ")
				|| !zend_mir_dump_id(dump, "sp", record.id)
				|| !zend_mir_dump_literal(dump, " source-payload ")
				|| !zend_mir_dump_id(
					dump, "vp", record.source_payload_id)
				|| !zend_mir_dump_literal(dump, " source-storage ")
				|| !zend_mir_dump_id(
					dump, "vs", record.source_storage_id)
				|| !zend_mir_dump_literal(dump, " reason ")
				|| !zend_mir_dump_u32(dump, (uint32_t) record.reason)
				|| !zend_mir_dump_literal(dump, " uniqueness ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.uniqueness_fact)
				|| !zend_mir_dump_literal(dump, " required ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.required)
				|| !zend_mir_dump_literal(dump, " result ")
				|| !zend_mir_dump_id(
					dump, "vp", record.result_payload_id)
				|| !zend_mir_dump_literal(
					dump, " clone-execution ")
				|| !zend_mir_dump_bool(
					dump, record.clone_execution_required)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
	ZEND_MIR_VALUE_COUNT(call_transfer)
	for (index = 0; index < count; index++) {
		zend_mir_call_transfer_ref record;
		if (!values->call_transfer_at(values->context, index, &record)
				|| !zend_mir_dump_literal(dump, "call-transfer ")
				|| !zend_mir_dump_id(
					dump, "cs", record.call_site_id)
				|| !zend_mir_dump_literal(dump, " parameter-modes ")
				|| !zend_mir_dump_u32(
					dump, record.parameter_modes.offset)
				|| !zend_mir_dump_literal(dump, "+")
				|| !zend_mir_dump_u32(
					dump, record.parameter_modes.count)
				|| !zend_mir_dump_literal(dump, " argument ")
				|| !zend_mir_dump_u32(dump, record.argument_ordinal)
				|| !zend_mir_dump_literal(dump, " storage ")
				|| !zend_mir_dump_id(
					dump, "vs", record.argument_storage_id)
				|| !zend_mir_dump_literal(
					dump, " argument-reference ")
				|| !zend_mir_dump_id(
					dump, "rc",
					record.argument_reference_cell_id)
				|| !zend_mir_dump_literal(dump, " action ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.argument_action)
				|| !zend_mir_dump_literal(dump, " return-storage ")
				|| !zend_mir_dump_id(
					dump, "vs", record.return_storage_id)
				|| !zend_mir_dump_literal(
					dump, " return-reference ")
				|| !zend_mir_dump_id(
					dump, "rc",
					record.return_reference_cell_id)
				|| !zend_mir_dump_literal(dump, " return-action ")
				|| !zend_mir_dump_u32(
					dump, (uint32_t) record.return_action)
				|| !zend_mir_dump_literal(dump, "\n")) {
			return false;
		}
	}
#undef ZEND_MIR_VALUE_COUNT
	return true;
}

static bool zend_mir_dump_invalid_record(
		zend_mir_dump_context *dump, const char *kind, uint32_t index)
{
	return zend_mir_dump_literal(dump, "invalid ")
		&& zend_mir_dump_literal(dump, kind)
		&& zend_mir_dump_literal(dump, " index ")
		&& zend_mir_dump_u32(dump, index)
		&& zend_mir_dump_literal(dump, "\n");
}

static bool zend_mir_dump_sorted(zend_mir_dump_context *dump, uint32_t count,
		zend_mir_dump_id_at_fn id_at, zend_mir_dump_record_fn dump_record, const char *kind)
{
	bool have_previous = false;
	uint32_t previous_id = 0;
	uint32_t previous_index = 0;
	uint32_t emitted = 0;

	while (emitted < count) {
		bool found = false;
		uint32_t best_id = 0;
		uint32_t best_index = 0;
		uint32_t index;

		for (index = 0; index < count; index++) {
			uint32_t id;
			if (!id_at(dump->view, index, &id)) {
				continue;
			}
			if (have_previous && (id < previous_id
					|| (id == previous_id && index <= previous_index))) {
				continue;
			}
			if (!found || id < best_id || (id == best_id && index < best_index)) {
				found = true;
				best_id = id;
				best_index = index;
			}
		}
		if (!found) {
			break;
		}
		if (!dump_record(dump, best_index)) {
			return false;
		}
		have_previous = true;
		previous_id = best_id;
		previous_index = best_index;
		emitted++;
	}
	if (emitted != count) {
		uint32_t index;
		for (index = 0; index < count; index++) {
			uint32_t ignored;
			if (!id_at(dump->view, index, &ignored)
					&& !zend_mir_dump_invalid_record(dump, kind, index)) {
				return false;
			}
		}
	}
	return true;
}

static bool zend_mir_dump_pool(zend_mir_dump_context *dump, uint32_t count,
		zend_mir_dump_record_fn dump_record)
{
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (!dump_record(dump, index)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_dump_view_is_complete(const zend_mir_view *view)
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
		&& view->predecessor_count != NULL && view->predecessor_at != NULL;
}

static bool zend_mir_dump_text_internal(
	const zend_mir_view *view, zend_mir_text_writer *writer,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_dump_context dump;

	memset(&dump, 0, sizeof(dump));
	dump.view = view;
	dump.writer = writer;
	dump.diagnostics = diagnostics;
	if (!zend_mir_dump_view_is_complete(view)) {
		zend_mir_dump_diagnostic(&dump, "incomplete ZNMIR view");
		return false;
	}
	if (!zend_mir_contract_is_compatible(view->contract_version)) {
		zend_mir_dump_diagnostic(&dump, "unsupported ZNMIR contract version");
		return false;
	}
	if ((view->value_fact_count == NULL) != (view->value_fact_at == NULL)) {
		zend_mir_dump_diagnostic(&dump, "incomplete scalar value-fact view");
		return false;
	}
	if (view->value_fact_count != NULL
			&& view->value_fact_count(view->context) > UINT32_C(1048576)) {
		zend_mir_dump_diagnostic(&dump, "scalar value-fact count exceeds hard limit");
		return false;
	}
	if (!zend_mir_dump_literal(&dump, "znmir 1.0 module ")
			|| !zend_mir_dump_id(&dump, "m", view->module_id(view->context))
			|| !zend_mir_dump_literal(&dump, "\n")
			|| !zend_mir_dump_sorted(&dump, view->function_count(view->context),
				zend_mir_function_id_at, zend_mir_dump_function, "function")
			|| !zend_mir_dump_sorted(&dump, view->block_count(view->context),
				zend_mir_block_id_at, zend_mir_dump_block, "block")
			|| !zend_mir_dump_sorted(&dump, view->value_count(view->context),
				zend_mir_value_id_at, zend_mir_dump_value, "value")
			|| !zend_mir_dump_sorted(&dump, view->constant_count(view->context),
				zend_mir_constant_id_at, zend_mir_dump_constant, "constant")
			|| (view->value_fact_count != NULL
				&& !zend_mir_dump_sorted(&dump,
					view->value_fact_count(view->context),
					zend_mir_value_fact_id_at,
					zend_mir_dump_value_fact, "fact"))
			|| !zend_mir_dump_sorted(&dump, view->source_position_count(view->context),
				zend_mir_source_id_at, zend_mir_dump_source, "source")
			|| !zend_mir_dump_pool(&dump, view->frame_slot_count(view->context), zend_mir_dump_slot)
			|| !zend_mir_dump_pool(&dump, view->root_count(view->context), zend_mir_dump_root)
			|| !zend_mir_dump_pool(&dump, view->cleanup_count(view->context), zend_mir_dump_cleanup)
			|| !zend_mir_dump_sorted(&dump, view->frame_state_count(view->context),
				zend_mir_frame_id_at, zend_mir_dump_frame, "frame")
			|| !zend_mir_dump_sorted(&dump, view->instruction_count(view->context),
				zend_mir_instruction_id_at, zend_mir_dump_instruction, "instruction")
			|| !zend_mir_dump_call_model(&dump)
			|| !zend_mir_dump_value_model(&dump)
			|| !zend_mir_dump_literal(&dump, "end\n")) {
		return false;
	}
	return true;
}

bool zend_mir_dump_text(const zend_mir_view *view,
	zend_mir_text_writer *writer, zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_dump_text_internal(view, writer, diagnostics);
}
