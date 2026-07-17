#include "tests/native/mir/text/mir_test_parser.h"
#include "tests/native/mir/text/text_fixtures.h"
#include "Zend/Native/MIR/Text/zend_mir_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_CAPACITY (ZEND_MIR_TEST_TEXT_MAX_BYTES + 1U)

typedef struct _fuzz_buffer {
	char *bytes;
	size_t length;
	size_t capacity;
} fuzz_buffer;

static bool fuzz_write(void *context, const char *bytes, size_t length)
{
	fuzz_buffer *buffer = (fuzz_buffer *) context;
	if (length > buffer->capacity - buffer->length) return false;
	memcpy(buffer->bytes + buffer->length, bytes, length);
	buffer->length += length;
	return true;
}

static uint64_t fuzz_random(uint64_t *state)
{
	uint64_t value = *state;
	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	*state = value;
	return value;
}

static bool fixture_bytes(const char *name, char *storage, size_t capacity, size_t *length)
{
	zend_mir_fixture_host host;
	fuzz_buffer buffer;
	zend_mir_text_writer writer;
	if (!zend_mir_text_build_fixture(name, &host)) return false;
	buffer.bytes = storage; buffer.length = 0; buffer.capacity = capacity;
	writer.context = &buffer; writer.write = fuzz_write;
	if (!zend_mir_dump_text(&host.view, &writer, NULL)) return false;
	*length = buffer.length;
	return true;
}

static void mutate(char *bytes, size_t *length, uint64_t *state)
{
	uint32_t mutations = (uint32_t) (fuzz_random(state) % 8U) + 1U;
	uint32_t index;
	for (index = 0; index < mutations; index++) {
		uint32_t operation = (uint32_t) (fuzz_random(state) % 6U);
		size_t position = *length == 0 ? 0 : (size_t) (fuzz_random(state) % *length);
		if (operation == 0 && *length != 0) bytes[position] ^= (char) (1U << (fuzz_random(state) % 8U));
		else if (operation == 1 && *length != 0) { memmove(bytes + position, bytes + position + 1, *length - position - 1); (*length)--; }
		else if (operation == 2 && *length < FUZZ_CAPACITY) { memmove(bytes + position + 1, bytes + position, *length - position); bytes[position] = (char) fuzz_random(state); (*length)++; }
		else if (operation == 3 && *length != 0) *length = position;
		else if (operation == 4 && *length != 0 && *length < FUZZ_CAPACITY) { size_t count = *length - position; if (count > FUZZ_CAPACITY - *length) count = FUZZ_CAPACITY - *length; memcpy(bytes + *length, bytes + position, count); *length += count; }
		else if (operation == 5 && *length != 0) bytes[position] = (char) 0xff;
	}
}

int main(int argc, char **argv)
{
	char *bases[ZEND_MIR_TEXT_FIXTURE_COUNT];
	size_t base_lengths[ZEND_MIR_TEXT_FIXTURE_COUNT];
	char *mutated;
	char *canonical;
	uint64_t state;
	uint32_t cases;
	uint32_t fixture;
	uint32_t iteration;
	if (argc != 3) { fprintf(stderr, "usage: %s SEED CASES\n", argv[0]); return 2; }
	state = strtoull(argv[1], NULL, 10); cases = (uint32_t) strtoul(argv[2], NULL, 10);
	if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
	mutated = (char *) malloc(FUZZ_CAPACITY); canonical = (char *) malloc(FUZZ_CAPACITY);
	if (mutated == NULL || canonical == NULL) return 2;
	for (fixture = 0; fixture < ZEND_MIR_TEXT_FIXTURE_COUNT; fixture++) {
		bases[fixture] = (char *) malloc(FUZZ_CAPACITY);
		if (bases[fixture] == NULL || !fixture_bytes(zend_mir_text_fixture_names[fixture], bases[fixture], FUZZ_CAPACITY, &base_lengths[fixture])) return 2;
	}
	for (iteration = 0; iteration < cases; iteration++) {
		zend_mir_fixture_host host;
		zend_mir_test_text_error error;
		size_t length;
		fixture = (uint32_t) (fuzz_random(&state) % ZEND_MIR_TEXT_FIXTURE_COUNT);
		length = base_lengths[fixture]; memcpy(mutated, bases[fixture], length); mutate(mutated, &length, &state);
		if (zend_mir_test_parse_text(mutated, length, &host, &error)) {
			fuzz_buffer buffer = { canonical, 0, FUZZ_CAPACITY };
			zend_mir_text_writer writer = { &buffer, fuzz_write };
			zend_mir_fixture_host reparsed;
			if (!zend_mir_dump_text(&host.view, &writer, NULL)
					|| !zend_mir_test_parse_text(canonical, buffer.length, &reparsed, &error)) return 1;
		}
	}
	for (fixture = 0; fixture < ZEND_MIR_TEXT_FIXTURE_COUNT; fixture++) free(bases[fixture]);
	free(mutated); free(canonical);
	printf("mutation fuzz passed: seed=%s cases=%u\n", argv[1], cases);
	return 0;
}
