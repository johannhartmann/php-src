#include "mir_test_parser.h"

#include <limits.h>
#include <string.h>

#define ZEND_MIR_TEST_ENUM_SIZE(type) \
	ZEND_MIR_STATIC_ASSERT(sizeof(type) == sizeof(uint32_t), #type " must hold invalid(u32)")
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_opcode);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_representation);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_constant_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_ownership_state);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_function_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_opline_phase);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_frame_slot_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_frame_slot_representation);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_materialization);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_frame_slot_ownership);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_cleanup_action);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_cleanup_state);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_continuation_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_suspend_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_resume_entry_kind);
ZEND_MIR_TEST_ENUM_SIZE(zend_mir_safepoint_class);
#undef ZEND_MIR_TEST_ENUM_SIZE

typedef struct _zend_mir_test_enum_entry {
	const char *label;
	uint32_t value;
} zend_mir_test_enum_entry;

#define ZEND_MIR_TEST_ENUM_ENTRY(symbol, label, value) { label, value },
static const zend_mir_test_enum_entry opcode_entries[] = {
	ZEND_MIR_OPCODE_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
	ZEND_MIR_SCALAR_OPCODE_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry representation_entries[] = {
	ZEND_MIR_REPRESENTATION_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry constant_kind_entries[] = {
	ZEND_MIR_CONSTANT_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry ownership_entries[] = {
	ZEND_MIR_OWNERSHIP_STATE_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry function_kind_entries[] = {
	ZEND_MIR_FUNCTION_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry opline_phase_entries[] = {
	ZEND_MIR_OPLINE_PHASE_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry slot_kind_entries[] = {
	ZEND_MIR_FRAME_SLOT_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry slot_representation_entries[] = {
	ZEND_MIR_FRAME_SLOT_REPRESENTATION_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry materialization_entries[] = {
	ZEND_MIR_MATERIALIZATION_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry slot_ownership_entries[] = {
	ZEND_MIR_FRAME_SLOT_OWNERSHIP_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry cleanup_action_entries[] = {
	ZEND_MIR_CLEANUP_ACTION_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry cleanup_state_entries[] = {
	ZEND_MIR_CLEANUP_STATE_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry continuation_entries[] = {
	ZEND_MIR_CONTINUATION_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry suspend_entries[] = {
	ZEND_MIR_SUSPEND_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry resume_entries[] = {
	ZEND_MIR_RESUME_ENTRY_KIND_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry safepoint_entries[] = {
	ZEND_MIR_SAFEPOINT_CLASS_CATALOG(ZEND_MIR_TEST_ENUM_ENTRY)
};
static const zend_mir_test_enum_entry scalar_type_entries[] = {
	{ "null", ZEND_MIR_SCALAR_TYPE_NULL },
	{ "i1", ZEND_MIR_SCALAR_TYPE_I1 },
	{ "i64", ZEND_MIR_SCALAR_TYPE_I64 },
	{ "f64", ZEND_MIR_SCALAR_TYPE_F64 },
};
static const zend_mir_test_enum_entry fact_provenance_entries[] = {
	{ "ssa", ZEND_MIR_FACT_PROVENANCE_SSA },
	{ "literal", ZEND_MIR_FACT_PROVENANCE_LITERAL },
	{ "range_analysis", ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS },
	{ "type_analysis", ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS },
	{ "contract", ZEND_MIR_FACT_PROVENANCE_CONTRACT },
};
#undef ZEND_MIR_TEST_ENUM_ENTRY

typedef struct _zend_mir_test_line {
	const char *begin;
	size_t length;
	size_t position;
	size_t absolute;
	uint32_t number;
	zend_mir_test_text_error *error;
	uint32_t *depth;
} zend_mir_test_line;

typedef struct _zend_mir_test_parser {
	const char *text;
	size_t length;
	size_t position;
	uint32_t line_count;
	zend_mir_fixture_host *host;
	zend_mir_test_text_error *error;
	zend_mir_block_id successors[ZEND_MIR_FIXTURE_MAX_BLOCKS][ZEND_MIR_FIXTURE_MAX_BLOCKS];
	uint32_t successor_counts[ZEND_MIR_FIXTURE_MAX_BLOCKS];
	zend_mir_block_id predecessors[ZEND_MIR_FIXTURE_MAX_BLOCKS][ZEND_MIR_FIXTURE_MAX_BLOCKS];
	uint32_t predecessor_counts[ZEND_MIR_FIXTURE_MAX_BLOCKS];
	zend_mir_value_fact_ref facts[ZEND_MIR_FIXTURE_MAX_VALUES];
	uint32_t fact_count;
	uint32_t section;
	uint32_t last_ids[12];
	bool have_last_id[12];
	uint32_t depth;
} zend_mir_test_parser;

static bool zend_mir_test_fail_at(zend_mir_test_text_error *error,
		zend_mir_test_text_error_code code, size_t offset, uint32_t line,
		uint32_t column, const char *message)
{
	size_t length;

	if (error != NULL && error->code == ZEND_MIR_TEST_TEXT_OK) {
		error->code = code;
		error->byte_offset = offset;
		error->line = line;
		error->column = column;
		length = strlen(message);
		if (length >= sizeof(error->message)) {
			length = sizeof(error->message) - 1;
		}
		memcpy(error->message, message, length);
		error->message[length] = '\0';
	}
	return false;
}

static bool zend_mir_test_fail(zend_mir_test_line *line,
		zend_mir_test_text_error_code code, const char *message)
{
	return zend_mir_test_fail_at(line->error, code, line->absolute + line->position,
		line->number, (uint32_t) line->position + 1, message);
}

static bool zend_mir_test_expect(zend_mir_test_line *line, const char *literal)
{
	size_t length = strlen(literal);

	if (length > line->length - line->position
			|| memcmp(line->begin + line->position, literal, length) != 0) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_SYNTAX, "unexpected token");
	}
	line->position += length;
	return true;
}

static bool zend_mir_test_end(zend_mir_test_line *line)
{
	if (line->position != line->length) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"trailing or noncanonical bytes");
	}
	return true;
}

static bool zend_mir_test_u32(zend_mir_test_line *line, uint32_t *out)
{
	uint64_t value = 0;
	size_t start = line->position;

	while (line->position < line->length) {
		unsigned char byte = (unsigned char) line->begin[line->position];
		if (byte < '0' || byte > '9') {
			break;
		}
		if (line->position != start && value == 0) {
			return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
				"leading zero in decimal integer");
		}
		value = value * 10 + (uint64_t) (byte - '0');
		if (value > UINT32_MAX) {
			return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT,
				"32-bit integer overflow");
		}
		line->position++;
	}
	if (line->position == start) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_SYNTAX, "expected decimal integer");
	}
	*out = (uint32_t) value;
	return true;
}

static bool zend_mir_test_hex(zend_mir_test_line *line, uint32_t digits, uint64_t *out)
{
	uint64_t value = 0;
	uint32_t index;

	if (digits > 16 || digits > line->length - line->position) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_TRUNCATED, "truncated hexadecimal integer");
	}
	for (index = 0; index < digits; index++) {
		unsigned char byte = (unsigned char) line->begin[line->position++];
		uint32_t nibble;
		if (byte >= '0' && byte <= '9') {
			nibble = byte - '0';
		} else if (byte >= 'a' && byte <= 'f') {
			nibble = byte - 'a' + 10;
		} else {
			return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
				"expected lowercase hexadecimal integer");
		}
		value = (value << 4) | nibble;
	}
	*out = value;
	return true;
}

static bool zend_mir_test_i64(zend_mir_test_line *line, int64_t *out)
{
	uint64_t value = 0;
	uint64_t limit = (uint64_t) INT64_MAX;
	size_t start;
	bool negative = false;

	if (line->position < line->length
			&& line->begin[line->position] == '-') {
		negative = true;
		limit++;
		line->position++;
	}
	start = line->position;
	while (line->position < line->length) {
		unsigned char byte = (unsigned char) line->begin[line->position];
		if (byte < '0' || byte > '9') {
			break;
		}
		if (line->position != start && value == 0) {
			return zend_mir_test_fail(line,
				ZEND_MIR_TEST_TEXT_NONCANONICAL,
				"leading zero in signed decimal integer");
		}
		if (value > (limit - (uint64_t) (byte - '0')) / 10) {
			return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT,
				"64-bit signed integer overflow");
		}
		value = value * 10 + (uint64_t) (byte - '0');
		line->position++;
	}
	if (line->position == start || (negative && value == 0)) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"invalid signed decimal integer");
	}
	if (negative) {
		*out = value == (UINT64_C(1) << 63)
			? INT64_MIN : -(int64_t) value;
	} else {
		*out = (int64_t) value;
	}
	return true;
}

static bool zend_mir_test_id(zend_mir_test_line *line, const char *prefix, uint32_t *out)
{
	if (line->length - line->position >= 7
			&& memcmp(line->begin + line->position, "invalid", 7) == 0) {
		line->position += 7;
		*out = ZEND_MIR_ID_INVALID;
		return true;
	}
	if (!zend_mir_test_expect(line, prefix) || !zend_mir_test_u32(line, out)) {
		return false;
	}
	if (*out == ZEND_MIR_ID_INVALID) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"invalid ID sentinel must be spelled invalid");
	}
	return true;
}

static bool zend_mir_test_bool(zend_mir_test_line *line, bool *out)
{
	if (line->length - line->position >= 4
			&& memcmp(line->begin + line->position, "true", 4) == 0) {
		line->position += 4;
		*out = true;
		return true;
	}
	if (line->length - line->position >= 5
			&& memcmp(line->begin + line->position, "false", 5) == 0) {
		line->position += 5;
		*out = false;
		return true;
	}
	return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_SYNTAX, "expected boolean");
}

static bool zend_mir_test_enum(zend_mir_test_line *line,
		const zend_mir_test_enum_entry *entries, size_t count, void *out)
{
	size_t index;
	uint32_t parsed;

	for (index = 0; index < count; index++) {
		size_t length = strlen(entries[index].label);
		if (length <= line->length - line->position
				&& memcmp(line->begin + line->position, entries[index].label, length) == 0
				&& (line->position + length == line->length
					|| line->begin[line->position + length] == ' '
					|| line->begin[line->position + length] == ':'
					|| line->begin[line->position + length] == ']')) {
			line->position += length;
			parsed = entries[index].value;
			memcpy(out, &parsed, sizeof(parsed));
			return true;
		}
	}
	return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_SYNTAX,
		"unknown or invalid enum label");
}

#define ZEND_MIR_TEST_ENUM(line, table, out) \
	zend_mir_test_enum((line), (table), sizeof(table) / sizeof((table)[0]), (out))

static bool zend_mir_test_list(zend_mir_test_line *line, const char *prefix,
		uint32_t *items, uint32_t capacity, uint32_t *count)
{
	if (!zend_mir_test_expect(line, "[")) {
		return false;
	}
	if (*line->depth >= ZEND_MIR_TEST_TEXT_MAX_NESTING) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT,
			"grammar nesting limit exceeded");
	}
	(*line->depth)++;
	*count = 0;
	if (line->position < line->length && line->begin[line->position] == ']') {
		line->position++;
		(*line->depth)--;
		return true;
	}
	for (;;) {
		if (*count >= capacity || *count >= ZEND_MIR_TEST_TEXT_MAX_LIST_ITEMS) {
			(*line->depth)--;
			return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT,
				"list item limit exceeded");
		}
		if (!zend_mir_test_id(line, prefix, &items[(*count)++])) {
			(*line->depth)--;
			return false;
		}
		if (line->position < line->length && line->begin[line->position] == ']') {
			line->position++;
			(*line->depth)--;
			return true;
		}
		if (!zend_mir_test_expect(line, ", ")) {
			(*line->depth)--;
			return false;
		}
	}
}

static bool zend_mir_test_duplicate(const uint32_t *ids, uint32_t count, uint32_t id)
{
	uint32_t index;
	for (index = 0; index < count; index++) {
		if (ids[index] == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_test_parse_header(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	uint32_t module_id;
	if (!zend_mir_test_expect(line, "znmir 1.0 module ")
			|| !zend_mir_test_id(line, "m", &module_id) || !zend_mir_test_end(line)) {
		return false;
	}
	if (module_id == ZEND_MIR_ID_INVALID) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE,
			"module ID must be valid");
	}
	zend_mir_fixture_host_init(parser->host, module_id);
	return true;
}

static bool zend_mir_test_parse_function(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_function_record record;
	uint64_t flags;
	uint32_t ids[ZEND_MIR_FIXTURE_MAX_FUNCTIONS];
	uint32_t index;
	if (parser->host->function_count >= ZEND_MIR_FIXTURE_MAX_FUNCTIONS) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "function limit exceeded");
	}
	for (index = 0; index < parser->host->function_count; index++) ids[index] = parser->host->functions[index].id;
	if (!zend_mir_test_expect(line, "function ") || !zend_mir_test_id(line, "f", &record.id)
			|| !zend_mir_test_expect(line, " symbol ") || !zend_mir_test_id(line, "s", &record.symbol_id)
			|| !zend_mir_test_expect(line, " entry ") || !zend_mir_test_id(line, "b", &record.entry_block_id)
			|| !zend_mir_test_expect(line, " flags 0x") || !zend_mir_test_hex(line, 8, &flags)
			|| !zend_mir_test_end(line)) return false;
	if (zend_mir_test_duplicate(ids, parser->host->function_count, record.id))
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate function ID");
	record.flags = (uint32_t) flags;
	parser->host->functions[parser->host->function_count++] = record;
	return true;
}

static bool zend_mir_test_parse_block(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_block_record record;
	uint32_t ids[ZEND_MIR_FIXTURE_MAX_BLOCKS];
	uint32_t index = parser->host->block_count;
	uint32_t scan;
	if (index >= ZEND_MIR_FIXTURE_MAX_BLOCKS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "block limit exceeded");
	for (scan = 0; scan < index; scan++) ids[scan] = parser->host->blocks[scan].id;
	if (!zend_mir_test_expect(line, "block ") || !zend_mir_test_id(line, "b", &record.id)
			|| !zend_mir_test_expect(line, " function ") || !zend_mir_test_id(line, "f", &record.function_id)
			|| !zend_mir_test_expect(line, " predecessors ")
			|| !zend_mir_test_list(line, "b", parser->predecessors[index], ZEND_MIR_FIXTURE_MAX_BLOCKS,
				&parser->predecessor_counts[index])
			|| !zend_mir_test_expect(line, " successors ")
			|| !zend_mir_test_list(line, "b", parser->successors[index], ZEND_MIR_FIXTURE_MAX_BLOCKS,
				&parser->successor_counts[index]) || !zend_mir_test_end(line)) return false;
	if (zend_mir_test_duplicate(ids, index, record.id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate block ID");
	parser->host->blocks[parser->host->block_count++] = record;
	return true;
}

static bool zend_mir_test_parse_value(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_value_record record;
	uint32_t ids[ZEND_MIR_FIXTURE_MAX_VALUES];
	uint32_t index;
	if (parser->host->value_count >= ZEND_MIR_FIXTURE_MAX_VALUES) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "value limit exceeded");
	for (index = 0; index < parser->host->value_count; index++) ids[index] = parser->host->values[index].id;
	if (!zend_mir_test_expect(line, "value ") || !zend_mir_test_id(line, "v", &record.id)
			|| !zend_mir_test_expect(line, " representation ") || !ZEND_MIR_TEST_ENUM(line, representation_entries, &record.representation)
			|| !zend_mir_test_expect(line, " ownership ") || !ZEND_MIR_TEST_ENUM(line, ownership_entries, &record.ownership)
			|| !zend_mir_test_end(line)) return false;
	if (zend_mir_test_duplicate(ids, parser->host->value_count, record.id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate value ID");
	parser->host->values[parser->host->value_count++] = record;
	return true;
}

static bool zend_mir_test_parse_constant(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_constant_record record;
	uint64_t payload;
	uint32_t index;
	if (parser->host->constant_count >= ZEND_MIR_FIXTURE_MAX_CONSTANTS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "constant limit exceeded");
	if (!zend_mir_test_expect(line, "constant ") || !zend_mir_test_id(line, "v", &record.value_id)
			|| !zend_mir_test_expect(line, " representation ") || !ZEND_MIR_TEST_ENUM(line, representation_entries, &record.representation)
			|| !zend_mir_test_expect(line, " kind ") || !ZEND_MIR_TEST_ENUM(line, constant_kind_entries, &record.kind)
			|| !zend_mir_test_expect(line, " payload 0x") || !zend_mir_test_hex(line, 16, &payload)
			|| !zend_mir_test_expect(line, " symbol ") || !zend_mir_test_id(line, "s", &record.symbol_id)
			|| !zend_mir_test_end(line)) return false;
	for (index = 0; index < parser->host->constant_count; index++) if (parser->host->constants[index].value_id == record.value_id) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate constant value ID");
	record.payload_bits = payload;
	if ((record.kind == ZEND_MIR_CONSTANT_KIND_NULL_VALUE
			|| record.kind == ZEND_MIR_CONSTANT_KIND_FALSE_VALUE
			|| record.kind == ZEND_MIR_CONSTANT_KIND_TRUE_VALUE)
			&& (record.payload_bits != 0 || record.symbol_id != ZEND_MIR_ID_INVALID)) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"payload-free constant has payload or symbol");
	}
	if ((record.kind == ZEND_MIR_CONSTANT_KIND_STRING_SYMBOL
			|| record.kind == ZEND_MIR_CONSTANT_KIND_SEMANTIC_POINTER_SYMBOL)
			&& (record.payload_bits != 0 || record.symbol_id == ZEND_MIR_ID_INVALID)) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"symbol constant has payload or missing symbol");
	}
	if ((record.kind == ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS
			|| record.kind == ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS)
			&& record.symbol_id != ZEND_MIR_ID_INVALID) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"numeric constant has a symbol");
	}
	parser->host->constants[parser->host->constant_count++] = record;
	return true;
}

static bool zend_mir_test_parse_fact(
		zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_value_fact_ref record;
	uint64_t flags;
	uint32_t index;
	const zend_mir_value_fact_flags known_flags =
		ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NONZERO
		| ZEND_MIR_VALUE_FACT_FINITE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;

	if (parser->fact_count >= ZEND_MIR_FIXTURE_MAX_VALUES) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT,
			"value-fact limit exceeded");
	}
	memset(&record, 0, sizeof(record));
	if (!zend_mir_test_expect(line, "fact ")
			|| !zend_mir_test_id(line, "vf", &record.id)
			|| !zend_mir_test_expect(line, " value ")
			|| !zend_mir_test_id(line, "v", &record.value_id)
			|| !zend_mir_test_expect(line, " type ")
			|| !ZEND_MIR_TEST_ENUM(line, scalar_type_entries, &record.exact_type)
			|| !zend_mir_test_expect(line, " flags 0x")
			|| !zend_mir_test_hex(line, 8, &flags)
			|| !zend_mir_test_expect(line, " range ")) {
		return false;
	}
	record.flags = (zend_mir_value_fact_flags) flags;
	if ((record.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0) {
		if (!zend_mir_test_i64(line, &record.integer_min)
				|| !zend_mir_test_expect(line, ":")
				|| !zend_mir_test_i64(line, &record.integer_max)) {
			return false;
		}
	} else if (!zend_mir_test_expect(line, "none")) {
		return false;
	}
	if (!zend_mir_test_expect(line, " provenance ")
			|| !ZEND_MIR_TEST_ENUM(line, fact_provenance_entries,
				&record.provenance)
			|| !zend_mir_test_expect(line, " source ")
			|| !zend_mir_test_id(line, "p",
				&record.provenance_source_position_id)
			|| !zend_mir_test_end(line)) {
		return false;
	}
	if ((record.flags & ~known_flags) != 0
			|| !zend_mir_scalar_type_is_exact(record.exact_type)
			|| ((record.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
				&& (record.exact_type != ZEND_MIR_SCALAR_TYPE_I64
					|| record.integer_min > record.integer_max))
			|| ((record.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) == 0
				&& (record.integer_min != 0 || record.integer_max != 0))
			|| ((record.flags & ZEND_MIR_VALUE_FACT_NONZERO) != 0
				&& (record.exact_type != ZEND_MIR_SCALAR_TYPE_I64
					|| ((record.flags
							& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
						&& record.integer_min <= 0
						&& record.integer_max >= 0)))
			|| ((record.flags & ZEND_MIR_VALUE_FACT_FINITE) != 0
				&& record.exact_type != ZEND_MIR_SCALAR_TYPE_F64)) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"invalid scalar value fact");
	}
	for (index = 0; index < parser->fact_count; index++) {
		if (parser->facts[index].id == record.id
				|| parser->facts[index].value_id == record.value_id) {
			return zend_mir_test_fail(line,
				ZEND_MIR_TEST_TEXT_DUPLICATE_ID,
				"duplicate value-fact ID or value");
		}
	}
	parser->facts[parser->fact_count++] = record;
	return true;
}

static bool zend_mir_test_parse_source(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_source_position_ref record;
	uint32_t index;
	if (parser->host->source_position_count >= ZEND_MIR_FIXTURE_MAX_SOURCE_POSITIONS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "source limit exceeded");
	if (!zend_mir_test_expect(line, "source ") || !zend_mir_test_id(line, "p", &record.id)
			|| !zend_mir_test_expect(line, " file ") || !zend_mir_test_id(line, "s", &record.file_symbol_id)
			|| !zend_mir_test_expect(line, " line ") || !zend_mir_test_u32(line, &record.line)
			|| !zend_mir_test_expect(line, " columns ") || !zend_mir_test_u32(line, &record.column_start)
			|| !zend_mir_test_expect(line, ":") || !zend_mir_test_u32(line, &record.column_end)
			|| !zend_mir_test_end(line)) return false;
	for (index = 0; index < parser->host->source_position_count; index++) if (parser->host->source_positions[index].id == record.id) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate source ID");
	parser->host->source_positions[parser->host->source_position_count++] = record;
	return true;
}

static bool zend_mir_test_parse_slot(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_frame_slot_ref record;
	if (parser->host->frame_slot_count >= ZEND_MIR_FIXTURE_MAX_FRAME_SLOTS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "slot limit exceeded");
	if (!zend_mir_test_expect(line, "slot ") || !zend_mir_test_u32(line, &record.slot_id)
			|| !zend_mir_test_expect(line, " value ") || !zend_mir_test_id(line, "v", &record.value_id)
			|| !zend_mir_test_expect(line, " index ") || !zend_mir_test_id(line, "", &record.index)
			|| !zend_mir_test_expect(line, " kind ") || !ZEND_MIR_TEST_ENUM(line, slot_kind_entries, &record.kind)
			|| !zend_mir_test_expect(line, " representation ") || !ZEND_MIR_TEST_ENUM(line, slot_representation_entries, &record.representation)
			|| !zend_mir_test_expect(line, " materialization ") || !ZEND_MIR_TEST_ENUM(line, materialization_entries, &record.materialization)
			|| !zend_mir_test_expect(line, " ownership ") || !ZEND_MIR_TEST_ENUM(line, slot_ownership_entries, &record.ownership)
			|| !zend_mir_test_expect(line, " rooted ") || !zend_mir_test_bool(line, &record.rooted)
			|| !zend_mir_test_expect(line, " cleanup ") || !zend_mir_test_bool(line, &record.cleanup_required)
			|| !zend_mir_test_end(line)) return false;
	parser->host->frame_slots[parser->host->frame_slot_count++] = record;
	return true;
}

static bool zend_mir_test_parse_root(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	uint32_t slot_id;
	if (parser->host->root_count >= ZEND_MIR_FIXTURE_MAX_ROOTS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "root limit exceeded");
	if (!zend_mir_test_expect(line, "root ") || !zend_mir_test_u32(line, &slot_id) || !zend_mir_test_end(line)) return false;
	parser->host->roots[parser->host->root_count++] = slot_id;
	return true;
}

static bool zend_mir_test_parse_cleanup(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_cleanup_ref record;
	if (parser->host->cleanup_count >= ZEND_MIR_FIXTURE_MAX_CLEANUPS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "cleanup limit exceeded");
	if (!zend_mir_test_expect(line, "cleanup ") || !zend_mir_test_u32(line, &record.slot_id)
			|| !zend_mir_test_expect(line, " action ") || !ZEND_MIR_TEST_ENUM(line, cleanup_action_entries, &record.action)
			|| !zend_mir_test_expect(line, " state ") || !ZEND_MIR_TEST_ENUM(line, cleanup_state_entries, &record.state)
			|| !zend_mir_test_end(line)) return false;
	parser->host->cleanups[parser->host->cleanup_count++] = record;
	return true;
}

static bool zend_mir_test_continuation(zend_mir_test_line *line, zend_mir_continuation_ref *record)
{
	return ZEND_MIR_TEST_ENUM(line, continuation_entries, &record->kind)
		&& zend_mir_test_expect(line, ":") && zend_mir_test_id(line, "fs", &record->frame_state_id)
		&& zend_mir_test_expect(line, ":") && zend_mir_test_id(line, "", &record->opline_index);
}

static bool zend_mir_test_span(zend_mir_test_line *line, zend_mir_span *span)
{
	return zend_mir_test_u32(line, &span->offset) && zend_mir_test_expect(line, "+")
		&& zend_mir_test_u32(line, &span->count);
}

static bool zend_mir_test_parse_frame(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_frame_state_ref r;
	uint32_t index;
	if (parser->host->frame_state_count >= ZEND_MIR_FIXTURE_MAX_FRAME_STATES) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "frame limit exceeded");
	if (!zend_mir_test_expect(line, "frame ") || !zend_mir_test_id(line, "fs", &r.id)
			|| !zend_mir_test_expect(line, " function ") || !zend_mir_test_id(line, "f", &r.function_id)
			|| !zend_mir_test_expect(line, " parent ") || !zend_mir_test_id(line, "fs", &r.parent_id)
			|| !zend_mir_test_expect(line, " function-kind ") || !ZEND_MIR_TEST_ENUM(line, function_kind_entries, &r.function_kind)
			|| !zend_mir_test_expect(line, " opline ") || !zend_mir_test_id(line, "", &r.opline_index)
			|| !zend_mir_test_expect(line, " phase ") || !ZEND_MIR_TEST_ENUM(line, opline_phase_entries, &r.opline_phase)
			|| !zend_mir_test_expect(line, " slots ") || !zend_mir_test_span(line, &r.slots)
			|| !zend_mir_test_expect(line, " roots ") || !zend_mir_test_span(line, &r.roots)
			|| !zend_mir_test_expect(line, " cleanups ") || !zend_mir_test_span(line, &r.cleanup_obligations)
			|| !zend_mir_test_expect(line, " return ") || !zend_mir_test_continuation(line, &r.return_continuation)
			|| !zend_mir_test_expect(line, " exception ") || !zend_mir_test_continuation(line, &r.exception_continuation)
			|| !zend_mir_test_expect(line, " bailout ") || !zend_mir_test_continuation(line, &r.bailout_continuation)
			|| !zend_mir_test_expect(line, " suspend ") || !ZEND_MIR_TEST_ENUM(line, suspend_entries, &r.suspend_kind)
			|| !zend_mir_test_expect(line, ":") || !zend_mir_test_id(line, "", &r.suspend_state_id)
			|| !zend_mir_test_expect(line, " code-version ") || !zend_mir_test_id(line, "", &r.code_version_id)
			|| !zend_mir_test_expect(line, " resume ") || !zend_mir_test_bool(line, &r.resume.allowed)
			|| !zend_mir_test_expect(line, ":") || !ZEND_MIR_TEST_ENUM(line, resume_entries, &r.resume.entry_kind)
			|| !zend_mir_test_expect(line, ":") || !zend_mir_test_id(line, "r", &r.resume.resume_id)
			|| !zend_mir_test_expect(line, ":") || !zend_mir_test_id(line, "", &r.resume.code_version_id)
			|| !zend_mir_test_expect(line, ":") || !zend_mir_test_id(line, "", &r.resume.target_opline_index)
			|| !zend_mir_test_expect(line, " safepoint ") || !ZEND_MIR_TEST_ENUM(line, safepoint_entries, &r.safepoint_class)
			|| !zend_mir_test_expect(line, " canonical ") || !zend_mir_test_bool(line, &r.canonical)
			|| !zend_mir_test_end(line)) return false;
	for (index = 0; index < parser->host->frame_state_count; index++) if (parser->host->frame_states[index].id == r.id) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate frame ID");
	parser->host->frame_states[parser->host->frame_state_count++] = r;
	return true;
}

static bool zend_mir_test_parse_instruction(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	zend_mir_instruction_record r;
	uint32_t operands[ZEND_MIR_TEST_TEXT_MAX_LIST_ITEMS];
	uint32_t operand_count;
	uint32_t index;
	uint64_t value;
	if (parser->host->instruction_count >= ZEND_MIR_FIXTURE_MAX_INSTRUCTIONS) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "instruction limit exceeded");
	if (!zend_mir_test_expect(line, "instruction ") || !zend_mir_test_id(line, "i", &r.id)
			|| !zend_mir_test_expect(line, " block ") || !zend_mir_test_id(line, "b", &r.block_id)
			|| !zend_mir_test_expect(line, " opcode ") || !ZEND_MIR_TEST_ENUM(line, opcode_entries, &r.opcode)
			|| !zend_mir_test_expect(line, " representation ") || !ZEND_MIR_TEST_ENUM(line, representation_entries, &r.representation)
			|| !zend_mir_test_expect(line, " result ") || !zend_mir_test_id(line, "v", &r.result_id)
			|| !zend_mir_test_expect(line, " operands ") || !zend_mir_test_list(line, "v", operands, ZEND_MIR_TEST_TEXT_MAX_LIST_ITEMS, &operand_count)
			|| !zend_mir_test_expect(line, " effects 0x") || !zend_mir_test_hex(line, 4, &value)) return false;
	r.effects = (zend_mir_effect_mask) value;
	if (!zend_mir_test_expect(line, " reads 0x") || !zend_mir_test_hex(line, 8, &value)) return false;
	r.reads = (zend_mir_memory_domain_mask) value;
	if (!zend_mir_test_expect(line, " writes 0x") || !zend_mir_test_hex(line, 8, &value)) return false;
	r.writes = (zend_mir_memory_domain_mask) value;
	if (!zend_mir_test_expect(line, " barriers 0x") || !zend_mir_test_hex(line, 2, &value)) return false;
	r.barriers = (zend_mir_barrier_mask) value;
	if (!zend_mir_test_expect(line, " ownership-actions 0x") || !zend_mir_test_hex(line, 4, &value)) return false;
	r.ownership_actions = (zend_mir_ownership_action_mask) value;
	if (!zend_mir_test_expect(line, " frame ") || !zend_mir_test_id(line, "fs", &r.frame_state_id)
			|| !zend_mir_test_expect(line, " source ") || !zend_mir_test_id(line, "p", &r.source_position_id)
			|| !zend_mir_test_end(line)) return false;
	for (index = 0; index < parser->host->instruction_count; index++) if (parser->host->instructions[index].id == r.id) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate instruction ID");
	if (parser->host->operand_count > ZEND_MIR_FIXTURE_MAX_OPERANDS - operand_count) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "operand limit exceeded");
	parser->host->instructions[parser->host->instruction_count++] = r;
	for (index = 0; index < operand_count; index++) {
		parser->host->operands[parser->host->operand_count].instruction_id = r.id;
		parser->host->operands[parser->host->operand_count++].value_id = operands[index];
	}
	return true;
}

static bool zend_mir_test_next_line(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	size_t start = parser->position;
	size_t position;
	if (start >= parser->length) return false;
	for (position = start; position < parser->length && parser->text[position] != '\n'; position++) {}
	if (position == parser->length) {
		zend_mir_test_fail_at(parser->error, ZEND_MIR_TEST_TEXT_TRUNCATED, position,
			parser->line_count + 1, (uint32_t) (position - start + 1), "missing final newline");
		return false;
	}
	if (position - start > ZEND_MIR_TEST_TEXT_MAX_LINE_BYTES) {
		zend_mir_test_fail_at(parser->error, ZEND_MIR_TEST_TEXT_LIMIT, start,
			parser->line_count + 1, 1, "line byte limit exceeded");
		return false;
	}
	if (++parser->line_count > ZEND_MIR_TEST_TEXT_MAX_LINES) {
		zend_mir_test_fail_at(parser->error, ZEND_MIR_TEST_TEXT_LIMIT, start,
			parser->line_count, 1, "line count limit exceeded");
		return false;
	}
	line->begin = parser->text + start;
	line->length = position - start;
	line->position = 0;
	line->absolute = start;
	line->number = parser->line_count;
	line->error = parser->error;
	line->depth = &parser->depth;
	parser->position = position + 1;
	return true;
}

static bool zend_mir_test_has_prefix(const zend_mir_test_line *line, const char *prefix)
{
	size_t length = strlen(prefix);
	return length <= line->length && memcmp(line->begin, prefix, length) == 0;
}

static bool zend_mir_test_accept_order(zend_mir_test_parser *parser,
		zend_mir_test_line *line, uint32_t section, bool sorted, uint32_t id)
{
	if (section < parser->section) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"record section is out of canonical order");
	}
	parser->section = section;
	if (sorted && parser->have_last_id[section] && id <= parser->last_ids[section]) {
		return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_NONCANONICAL,
			"record IDs are not in ascending order");
	}
	if (sorted) {
		parser->have_last_id[section] = true;
		parser->last_ids[section] = id;
	}
	return true;
}

static bool zend_mir_test_has_function(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	if (id == ZEND_MIR_ID_INVALID) return true;
	for (index = 0; index < host->function_count; index++) if (host->functions[index].id == id) return true;
	return false;
}

static bool zend_mir_test_has_block(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	if (id == ZEND_MIR_ID_INVALID) return true;
	for (index = 0; index < host->block_count; index++) if (host->blocks[index].id == id) return true;
	return false;
}

static bool zend_mir_test_has_value(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	if (id == ZEND_MIR_ID_INVALID) return true;
	for (index = 0; index < host->value_count; index++) if (host->values[index].id == id) return true;
	return false;
}

static bool zend_mir_test_has_frame(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	if (id == ZEND_MIR_ID_INVALID) return true;
	for (index = 0; index < host->frame_state_count; index++) if (host->frame_states[index].id == id) return true;
	return false;
}

static bool zend_mir_test_has_source(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	if (id == ZEND_MIR_ID_INVALID) return true;
	for (index = 0; index < host->source_position_count; index++) if (host->source_positions[index].id == id) return true;
	return false;
}

static bool zend_mir_test_has_slot(const zend_mir_fixture_host *host, uint32_t id)
{
	uint32_t index;
	for (index = 0; index < host->frame_slot_count; index++) if (host->frame_slots[index].slot_id == id) return true;
	return false;
}

static bool zend_mir_test_span_valid(zend_mir_span span, uint32_t count)
{
	return span.offset <= count && span.count <= count - span.offset;
}

static bool zend_mir_test_finalize(zend_mir_test_parser *parser, zend_mir_test_line *line)
{
	uint32_t block_index;
	uint32_t edge_index;
	uint32_t function_index;
	uint32_t index;
	uint32_t next_successor[ZEND_MIR_FIXTURE_MAX_BLOCKS] = { 0 };
	uint32_t next_predecessor[ZEND_MIR_FIXTURE_MAX_BLOCKS] = { 0 };
	uint32_t total_edges = 0;
	uint32_t total_predecessors = 0;
	for (function_index = 0; function_index < parser->host->function_count; function_index++) {
		zend_mir_function_record *function = &parser->host->functions[function_index];
		if (function->id == ZEND_MIR_ID_INVALID || function->symbol_id == ZEND_MIR_ID_INVALID
				|| function->entry_block_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_block(parser->host, function->entry_block_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "function has invalid identity or entry block");
	}
	for (block_index = 0; block_index < parser->host->block_count; block_index++) {
		if (parser->host->blocks[block_index].id == ZEND_MIR_ID_INVALID
				|| parser->host->blocks[block_index].function_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_function(parser->host, parser->host->blocks[block_index].function_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "block has invalid identity or function");
	}
	for (index = 0; index < parser->host->value_count; index++) {
		if (parser->host->values[index].id == ZEND_MIR_ID_INVALID) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "value ID must be valid");
	}
	for (index = 0; index < parser->host->constant_count; index++) {
		if (parser->host->constants[index].value_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_value(parser->host, parser->host->constants[index].value_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "constant value does not exist");
	}
	for (index = 0; index < parser->fact_count; index++) {
		if (parser->facts[index].id == ZEND_MIR_ID_INVALID
				|| parser->facts[index].value_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_value(
					parser->host, parser->facts[index].value_id)
				|| !zend_mir_test_has_source(parser->host,
					parser->facts[index]
						.provenance_source_position_id)) {
			return zend_mir_test_fail(line,
				ZEND_MIR_TEST_TEXT_REFERENCE,
				"invalid scalar value-fact reference");
		}
	}
	for (index = 0; index < parser->host->source_position_count; index++) {
		if (parser->host->source_positions[index].id == ZEND_MIR_ID_INVALID
				|| parser->host->source_positions[index].file_symbol_id == ZEND_MIR_ID_INVALID) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "source identity must be valid");
	}
	for (index = 0; index < parser->host->frame_slot_count; index++) {
		if (!zend_mir_test_has_value(parser->host, parser->host->frame_slots[index].value_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "frame slot value does not exist");
	}
	for (index = 0; index < parser->host->root_count; index++) {
		if (!zend_mir_test_has_slot(parser->host, parser->host->roots[index])) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "root slot does not exist");
	}
	for (index = 0; index < parser->host->cleanup_count; index++) {
		if (!zend_mir_test_has_slot(parser->host, parser->host->cleanups[index].slot_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "cleanup slot does not exist");
	}
	for (index = 0; index < parser->host->frame_state_count; index++) {
		zend_mir_frame_state_ref *frame = &parser->host->frame_states[index];
		if (frame->id == ZEND_MIR_ID_INVALID || frame->function_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_function(parser->host, frame->function_id)
				|| !zend_mir_test_has_frame(parser->host, frame->parent_id)
				|| !zend_mir_test_has_frame(parser->host, frame->return_continuation.frame_state_id)
				|| !zend_mir_test_has_frame(parser->host, frame->exception_continuation.frame_state_id)
				|| !zend_mir_test_has_frame(parser->host, frame->bailout_continuation.frame_state_id)
				|| !zend_mir_test_span_valid(frame->slots, parser->host->frame_slot_count)
				|| !zend_mir_test_span_valid(frame->roots, parser->host->root_count)
				|| !zend_mir_test_span_valid(frame->cleanup_obligations, parser->host->cleanup_count)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "invalid frame-state reference or span");
	}
	for (index = 0; index < parser->host->instruction_count; index++) {
		zend_mir_instruction_record *instruction = &parser->host->instructions[index];
		if (instruction->id == ZEND_MIR_ID_INVALID || instruction->block_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_block(parser->host, instruction->block_id)
				|| !zend_mir_test_has_value(parser->host, instruction->result_id)
				|| !zend_mir_test_has_frame(parser->host, instruction->frame_state_id)
				|| !zend_mir_test_has_source(parser->host, instruction->source_position_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "invalid instruction reference");
	}
	for (index = 0; index < parser->host->operand_count; index++) {
		if (parser->host->operands[index].value_id == ZEND_MIR_ID_INVALID
				|| !zend_mir_test_has_value(parser->host, parser->host->operands[index].value_id)) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "instruction operand value does not exist");
	}
	for (block_index = 0; block_index < parser->host->block_count; block_index++) {
		if (parser->successor_counts[block_index] > ZEND_MIR_FIXTURE_MAX_EDGES - total_edges) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_LIMIT, "edge limit exceeded");
		total_edges += parser->successor_counts[block_index];
		total_predecessors += parser->predecessor_counts[block_index];
		for (edge_index = 0; edge_index < parser->successor_counts[block_index]; edge_index++) {
			uint32_t earlier;
			for (earlier = 0; earlier < edge_index; earlier++) if (parser->successors[block_index][earlier] == parser->successors[block_index][edge_index]) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_DUPLICATE_ID, "duplicate CFG edge");
		}
	}
	if (total_edges != total_predecessors) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "edge predecessor/successor count mismatch");
	while (parser->host->edge_count < total_edges) {
		bool emitted = false;
		for (block_index = 0; block_index < parser->host->block_count; block_index++) {
			uint32_t target_index;
			zend_mir_block_id from;
			zend_mir_block_id to;
			if (next_successor[block_index] >= parser->successor_counts[block_index]) continue;
			from = parser->host->blocks[block_index].id;
			to = parser->successors[block_index][next_successor[block_index]];
			for (target_index = 0; target_index < parser->host->block_count; target_index++) if (parser->host->blocks[target_index].id == to) break;
			if (target_index == parser->host->block_count) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "successor block does not exist");
			if (next_predecessor[target_index] >= parser->predecessor_counts[target_index]
					|| parser->predecessors[target_index][next_predecessor[target_index]] != from) continue;
			parser->host->edges[parser->host->edge_count].from = from;
			parser->host->edges[parser->host->edge_count++].to = to;
			next_successor[block_index]++;
			next_predecessor[target_index]++;
			emitted = true;
			break;
		}
		if (!emitted) return zend_mir_test_fail(line, ZEND_MIR_TEST_TEXT_REFERENCE, "CFG edge orders cannot be represented by fixture host");
	}
	for (function_index = 0; function_index < parser->host->function_count; function_index++) parser->host->sealed[function_index] = true;
	return true;
}

bool zend_mir_test_parse_text(const char *text, size_t length,
		zend_mir_fixture_host *host, zend_mir_test_text_error *error)
{
	zend_mir_test_parser parser;
	zend_mir_test_line line;
	zend_mir_fixture_host parsed_host;
	bool saw_end = false;
	size_t index;
	uint32_t byte_line = 1;
	uint32_t byte_column = 1;
	if (error != NULL) memset(error, 0, sizeof(*error));
	if (text == NULL || host == NULL) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_SYNTAX, 0, 1, 1, "null parser input");
	if (length > ZEND_MIR_TEST_TEXT_MAX_BYTES) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_LIMIT, 0, 1, 1, "input byte limit exceeded");
	if (length == 0) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_TRUNCATED, 0, 1, 1, "empty input");
	for (index = 0; index < length; index++) {
		unsigned char byte = (unsigned char) text[index];
		if (byte != '\n' && (byte < 0x20 || byte > 0x7e)) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_INVALID_BYTE, index, byte_line, byte_column, "non-ASCII or control byte");
		if (byte == '\n') { byte_line++; byte_column = 1; }
		else byte_column++;
	}
	memset(&parser, 0, sizeof(parser));
	parser.text = text; parser.length = length; parser.host = &parsed_host; parser.error = error;
	if (!zend_mir_test_next_line(&parser, &line) || !zend_mir_test_parse_header(&parser, &line)) return false;
	while (parser.position < parser.length) {
		if (!zend_mir_test_next_line(&parser, &line)) return false;
		if (line.length == 3 && memcmp(line.begin, "end", 3) == 0) { saw_end = true; break; }
		if (zend_mir_test_has_prefix(&line, "function ")) {
			if (!zend_mir_test_parse_function(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 1, true,
						parser.host->functions[parser.host->function_count - 1].id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "block ")) {
			if (!zend_mir_test_parse_block(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 2, true,
						parser.host->blocks[parser.host->block_count - 1].id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "value ")) {
			if (!zend_mir_test_parse_value(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 3, true,
						parser.host->values[parser.host->value_count - 1].id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "constant ")) {
			if (!zend_mir_test_parse_constant(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 4, true,
						parser.host->constants[parser.host->constant_count - 1].value_id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "fact ")) {
			if (!zend_mir_test_parse_fact(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 5, true,
						parser.facts[parser.fact_count - 1].value_id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "source ")) {
			if (!zend_mir_test_parse_source(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 6, true,
						parser.host->source_positions[parser.host->source_position_count - 1].id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "slot ")) {
			if (!zend_mir_test_parse_slot(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 7, false, 0)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "root ")) {
			if (!zend_mir_test_parse_root(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 8, false, 0)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "cleanup ")) {
			if (!zend_mir_test_parse_cleanup(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 9, false, 0)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "frame ")) {
			if (!zend_mir_test_parse_frame(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 10, true,
						parser.host->frame_states[parser.host->frame_state_count - 1].id)) return false;
		}
		else if (zend_mir_test_has_prefix(&line, "instruction ")) {
			if (!zend_mir_test_parse_instruction(&parser, &line)
					|| !zend_mir_test_accept_order(&parser, &line, 11, true,
						parser.host->instructions[parser.host->instruction_count - 1].id)) return false;
		}
		else return zend_mir_test_fail(&line, ZEND_MIR_TEST_TEXT_SYNTAX, "unknown record kind");
	}
	if (!saw_end) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_TRUNCATED, length, parser.line_count + 1, 1, "missing end record");
	if (parser.position != parser.length) return zend_mir_test_fail_at(error, ZEND_MIR_TEST_TEXT_NONCANONICAL, parser.position, parser.line_count + 1, 1, "bytes after end record");
	if (!zend_mir_test_finalize(&parser, &line)) return false;
	*host = parsed_host;
	host->view.context = host;
	host->mutator.context = host;
	return true;
}
