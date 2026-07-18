#include "mir_test_parser.h"
#include "text_fixtures.h"
#include "Zend/Native/MIR/Text/zend_mir_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _test_buffer {
	char *bytes;
	size_t length;
	size_t capacity;
	size_t chunk;
	size_t fail_after;
} test_buffer;

typedef struct _test_diagnostics {
	uint32_t count;
	zend_mir_diagnostic_code last_code;
} test_diagnostics;

static bool capture_diagnostic(void *context, const zend_mir_diagnostic *diagnostic)
{
	test_diagnostics *capture = (test_diagnostics *) context;
	capture->count++;
	capture->last_code = diagnostic->code;
	return true;
}

static bool buffer_write(void *context, const char *bytes, size_t length)
{
	test_buffer *buffer = (test_buffer *) context;
	size_t offset = 0;
	if (buffer->fail_after != SIZE_MAX && length > buffer->fail_after - buffer->length) return false;
	if (length > SIZE_MAX - buffer->length) return false;
	if (buffer->length + length == SIZE_MAX) return false;
	if (buffer->length + length + 1 > buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
		char *grown;
		while (capacity < buffer->length + length + 1) {
			if (capacity > SIZE_MAX / 2) return false;
			capacity *= 2;
		}
		grown = (char *) realloc(buffer->bytes, capacity);
		if (grown == NULL) return false;
		buffer->bytes = grown;
		buffer->capacity = capacity;
	}
	while (offset < length) {
		size_t part = length - offset;
		if (buffer->chunk != 0 && part > buffer->chunk) part = buffer->chunk;
		memcpy(buffer->bytes + buffer->length, bytes + offset, part);
		buffer->length += part;
		offset += part;
	}
	buffer->bytes[buffer->length] = '\0';
	return true;
}

static bool dump_host(const zend_mir_fixture_host *host, size_t chunk,
		size_t fail_after, test_buffer *buffer)
{
	zend_mir_text_writer writer;
	memset(buffer, 0, sizeof(*buffer));
	buffer->chunk = chunk;
	buffer->fail_after = fail_after;
	writer.context = buffer;
	writer.write = buffer_write;
	return zend_mir_dump_text(&host->view, &writer, NULL);
}

static void buffer_release(test_buffer *buffer)
{
	free(buffer->bytes);
	memset(buffer, 0, sizeof(*buffer));
}

static int fail(const char *message)
{
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

#define REVERSE_RECORDS(array, count, type) do { \
	uint32_t reverse_index; \
	for (reverse_index = 0; reverse_index < (count) / 2; reverse_index++) { \
		type temporary = (array)[reverse_index]; \
		(array)[reverse_index] = (array)[(count) - reverse_index - 1]; \
		(array)[(count) - reverse_index - 1] = temporary; \
	} \
} while (0)

static void reverse_nonsemantic_storage(zend_mir_fixture_host *host)
{
	REVERSE_RECORDS(host->functions, host->function_count, zend_mir_function_record);
	REVERSE_RECORDS(host->blocks, host->block_count, zend_mir_block_record);
	REVERSE_RECORDS(host->values, host->value_count, zend_mir_value_record);
	REVERSE_RECORDS(host->constants, host->constant_count, zend_mir_constant_record);
	REVERSE_RECORDS(host->source_positions, host->source_position_count, zend_mir_source_position_ref);
	REVERSE_RECORDS(host->frame_states, host->frame_state_count, zend_mir_frame_state_ref);
	REVERSE_RECORDS(host->instructions, host->instruction_count, zend_mir_instruction_record);
}

static int test_fixture(const char *name)
{
	zend_mir_fixture_host original;
	zend_mir_fixture_host parsed;
	zend_mir_fixture_host reordered;
	zend_mir_test_text_error error;
	test_buffer baseline;
	test_buffer repeat;
	test_buffer roundtrip;
	test_buffer chunked;
	test_buffer reverse;
	if (!zend_mir_text_build_fixture(name, &original) || !dump_host(&original, 0, SIZE_MAX, &baseline)) return fail("fixture dump failed");
	if (!dump_host(&original, 0, SIZE_MAX, &repeat)) return fail("repeat dump failed");
	if (baseline.length != repeat.length || memcmp(baseline.bytes, repeat.bytes, baseline.length) != 0) return fail("repeat dump differs");
	if (!dump_host(&original, 1, SIZE_MAX, &chunked)) return fail("chunked dump failed");
	if (baseline.length != chunked.length || memcmp(baseline.bytes, chunked.bytes, baseline.length) != 0) return fail("writer chunking changed bytes");
	if (!zend_mir_test_parse_text(baseline.bytes, baseline.length, &parsed, &error)) {
		fprintf(stderr, "parse error %u at %zu (%u:%u): %s\n", (unsigned) error.code,
			error.byte_offset, error.line, error.column, error.message);
		return fail("canonical dump did not parse");
	}
	if (!dump_host(&parsed, 7, SIZE_MAX, &roundtrip)) return fail("roundtrip dump failed");
	if (baseline.length != roundtrip.length || memcmp(baseline.bytes, roundtrip.bytes, baseline.length) != 0) return fail("roundtrip bytes differ");
	reordered = original;
	reordered.view.context = &reordered;
	reordered.mutator.context = &reordered;
	reverse_nonsemantic_storage(&reordered);
	if (!dump_host(&reordered, 3, SIZE_MAX, &reverse)) return fail("reordered fixture dump failed");
	if (baseline.length != reverse.length || memcmp(baseline.bytes, reverse.bytes, baseline.length) != 0) return fail("record insertion order changed bytes");
	buffer_release(&baseline); buffer_release(&repeat); buffer_release(&roundtrip);
	buffer_release(&chunked); buffer_release(&reverse);
	return 0;
}

static int expect_parse_error(const char *text, size_t length,
		zend_mir_test_text_error_code expected)
{
	zend_mir_fixture_host host;
	zend_mir_test_text_error error;
	zend_mir_fixture_host_init(&host, 12345);
	if (zend_mir_test_parse_text(text, length, &host, &error)) return fail("invalid text parsed successfully");
	if (error.code != expected || error.line == 0 || error.column == 0) return fail("wrong or imprecise parser diagnostic");
	if (host.module_id != 12345 || host.function_count != 0) return fail("failed parse published partial fixture state");
	return 0;
}

static int test_failures(void)
{
	static const char valid[] = "znmir 1.0 module m1\nend\n";
	static const char duplicate[] =
		"znmir 1.0 module m1\n"
		"value v0 representation i64 ownership owned\n"
		"value v0 representation i64 ownership owned\nend\n";
	static const char overflow[] = "znmir 1.0 module m4294967296\nend\n";
	static const char numeric_sentinel[] = "znmir 1.0 module m4294967295\nend\n";
	static const char trailing[] = "znmir 1.0 module m1\nend\nend\n";
	static const char out_of_order[] =
		"znmir 1.0 module m1\n"
		"block b0 function f0 predecessors [] successors []\n"
		"function f0 symbol s1 entry b0 flags 0x00000000\nend\n";
	static const char unknown_field[] =
		"znmir 1.0 module m1\n"
		"value v0 representation i64 ownership owned mystery 0\nend\n";
	static const char duplicate_field[] =
		"znmir 1.0 module m1\n"
		"value v0 representation i64 ownership owned ownership owned\nend\n";
	char invalid_byte[sizeof(valid)];
	char count_input[16384];
	char list_input[4096];
	size_t count_length;
	size_t list_length;
	uint32_t count_index;
	zend_mir_fixture_host host;
	test_buffer failed_writer;
	test_diagnostics capture = { 0, ZEND_MIR_DIAGNOSTIC_NONE };
	zend_mir_diagnostic_sink sink;
	zend_mir_text_writer writer;
	memcpy(invalid_byte, valid, sizeof(valid)); invalid_byte[8] = (char) 0xff;
	if (expect_parse_error(valid, sizeof(valid) - 2, ZEND_MIR_TEST_TEXT_TRUNCATED)) return 1;
	if (expect_parse_error(duplicate, sizeof(duplicate) - 1, ZEND_MIR_TEST_TEXT_DUPLICATE_ID)) return 1;
	if (expect_parse_error(overflow, sizeof(overflow) - 1, ZEND_MIR_TEST_TEXT_LIMIT)) return 1;
	if (expect_parse_error(numeric_sentinel, sizeof(numeric_sentinel) - 1, ZEND_MIR_TEST_TEXT_NONCANONICAL)) return 1;
	if (expect_parse_error(trailing, sizeof(trailing) - 1, ZEND_MIR_TEST_TEXT_NONCANONICAL)) return 1;
	if (expect_parse_error(out_of_order, sizeof(out_of_order) - 1, ZEND_MIR_TEST_TEXT_NONCANONICAL)) return 1;
	if (expect_parse_error(unknown_field, sizeof(unknown_field) - 1,
			ZEND_MIR_TEST_TEXT_NONCANONICAL)) return 1;
	if (expect_parse_error(duplicate_field, sizeof(duplicate_field) - 1,
			ZEND_MIR_TEST_TEXT_NONCANONICAL)) return 1;
	if (expect_parse_error(invalid_byte, sizeof(valid) - 1, ZEND_MIR_TEST_TEXT_INVALID_BYTE)) return 1;
	if (expect_parse_error(valid, ZEND_MIR_TEST_TEXT_MAX_BYTES + 1U, ZEND_MIR_TEST_TEXT_LIMIT)) return 1;
	count_length = (size_t) snprintf(count_input, sizeof(count_input), "znmir 1.0 module m1\n");
	for (count_index = 0; count_index <= ZEND_MIR_FIXTURE_MAX_VALUES; count_index++) {
		int written = snprintf(count_input + count_length, sizeof(count_input) - count_length,
			"value v%u representation i64 ownership owned\n", count_index);
		if (written < 0 || (size_t) written >= sizeof(count_input) - count_length) return fail("count-limit fixture overflow");
		count_length += (size_t) written;
	}
	memcpy(count_input + count_length, "end\n", 4); count_length += 4;
	if (expect_parse_error(count_input, count_length, ZEND_MIR_TEST_TEXT_LIMIT)) return 1;
	count_length = (size_t) snprintf(count_input, sizeof(count_input),
		"znmir 1.0 module m1\n");
	for (count_index = 0; count_index <= ZEND_MIR_FIXTURE_MAX_VALUES; count_index++) {
		int written = snprintf(count_input + count_length,
			sizeof(count_input) - count_length,
			"fact vf%u value v%u type i64 flags 0x00000009 range 0:0 "
			"provenance range_analysis source invalid\n",
			count_index, count_index);
		if (written < 0
				|| (size_t) written >= sizeof(count_input) - count_length) {
			return fail("fact-count-limit fixture overflow");
		}
		count_length += (size_t) written;
	}
	memcpy(count_input + count_length, "end\n", 4); count_length += 4;
	if (expect_parse_error(count_input, count_length,
			ZEND_MIR_TEST_TEXT_LIMIT)) return 1;
	list_length = (size_t) snprintf(list_input, sizeof(list_input),
		"znmir 1.0 module m1\nfunction f0 symbol s1 entry b0 flags 0x00000000\n"
		"block b0 function f0 predecessors [] successors []\n"
		"value v0 representation i64 ownership owned\n"
		"instruction i0 block b0 opcode return representation void result invalid operands [");
	for (count_index = 0; count_index <= ZEND_MIR_TEST_TEXT_MAX_LIST_ITEMS; count_index++) {
		int written = snprintf(list_input + list_length, sizeof(list_input) - list_length,
			"%sv0", count_index == 0 ? "" : ", ");
		if (written < 0 || (size_t) written >= sizeof(list_input) - list_length) return fail("list-limit fixture overflow");
		list_length += (size_t) written;
	}
	{
		static const char suffix[] = "] effects 0x0000 reads 0x00000000 writes 0x00000000 barriers 0x00 ownership-actions 0x0000 frame invalid source invalid\nend\n";
		if (sizeof(suffix) - 1 > sizeof(list_input) - list_length) return fail("list-limit suffix overflow");
		memcpy(list_input + list_length, suffix, sizeof(suffix) - 1); list_length += sizeof(suffix) - 1;
	}
	if (expect_parse_error(list_input, list_length, ZEND_MIR_TEST_TEXT_LIMIT)) return 1;
	if (!zend_mir_text_build_fixture("linear", &host)) return fail("linear fixture unavailable");
	memset(&failed_writer, 0, sizeof(failed_writer)); failed_writer.fail_after = 10;
	writer.context = &failed_writer; writer.write = buffer_write;
	sink.context = &capture; sink.emit = capture_diagnostic; sink.limit = 1; sink.emitted = 0;
	if (zend_mir_dump_text(&host.view, &writer, &sink)) return fail("writer failure was reported as success");
	if (capture.count != 1 || capture.last_code != ZEND_MIR_DIAGNOSTIC_INVALID_TEXT) return fail("writer failure diagnostic was not propagated");
	buffer_release(&failed_writer);
	return 0;
}

static int test_invalid_visibility(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host parsed;
	zend_mir_test_text_error error;
	test_buffer first;
	if (!zend_mir_text_build_fixture("linear", &host)) return fail("linear fixture unavailable");
	host.values[0].ownership = (zend_mir_ownership_state) 77;
	if (!dump_host(&host, 0, SIZE_MAX, &first)) return fail("invalid IR was not dumpable");
	if (first.length < 11 || strstr(first.bytes, "invalid(77)") == NULL) return fail("invalid enum not visible");
	zend_mir_fixture_host_init(&parsed, 12345);
	if (zend_mir_test_parse_text(first.bytes, first.length, &parsed, &error)
			|| error.code != ZEND_MIR_TEST_TEXT_SYNTAX || parsed.module_id != 12345) return fail("strict parser accepted or normalized invalid enum");
	buffer_release(&first);
	return 0;
}

static int run_self_tests(void)
{
	uint32_t index;
	for (index = 0; index < ZEND_MIR_TEXT_FIXTURE_COUNT; index++) {
		if (test_fixture(zend_mir_text_fixture_names[index])) return 1;
	}
	if (test_failures() || test_invalid_visibility()) return 1;
	puts("all znmir-text-v1 C tests passed");
	return 0;
}

static int emit_fixture(const char *name)
{
	zend_mir_fixture_host host;
	test_buffer buffer;
	if (!zend_mir_text_build_fixture(name, &host) || !dump_host(&host, 0, SIZE_MAX, &buffer)) return fail("could not emit fixture");
	if (fwrite(buffer.bytes, 1, buffer.length, stdout) != buffer.length) return fail("stdout write failed");
	buffer_release(&buffer);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return run_self_tests();
	if (argc == 3 && strcmp(argv[1], "--emit") == 0) return emit_fixture(argv[2]);
	fprintf(stderr, "usage: %s --self-test | --emit FIXTURE\n", argv[0]);
	return 2;
}
