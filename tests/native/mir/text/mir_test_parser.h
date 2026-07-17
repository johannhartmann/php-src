/* Test-only parser for canonical znmir-text-v1 fixtures. */

#ifndef ZEND_MIR_TEST_TEXT_PARSER_H
#define ZEND_MIR_TEST_TEXT_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tests/native/mir/contracts/fixture_host.h"

#define ZEND_MIR_TEST_TEXT_MAX_BYTES (1024U * 1024U)
#define ZEND_MIR_TEST_TEXT_MAX_LINES 4096U
#define ZEND_MIR_TEST_TEXT_MAX_LINE_BYTES 4096U
#define ZEND_MIR_TEST_TEXT_MAX_LIST_ITEMS 256U
#define ZEND_MIR_TEST_TEXT_MAX_DIAGNOSTICS 16U
#define ZEND_MIR_TEST_TEXT_MAX_NESTING 1U

typedef enum _zend_mir_test_text_error_code {
	ZEND_MIR_TEST_TEXT_OK = 0,
	ZEND_MIR_TEST_TEXT_INVALID_BYTE,
	ZEND_MIR_TEST_TEXT_TRUNCATED,
	ZEND_MIR_TEST_TEXT_LIMIT,
	ZEND_MIR_TEST_TEXT_SYNTAX,
	ZEND_MIR_TEST_TEXT_DUPLICATE_ID,
	ZEND_MIR_TEST_TEXT_REFERENCE,
	ZEND_MIR_TEST_TEXT_NONCANONICAL
} zend_mir_test_text_error_code;

typedef struct _zend_mir_test_text_error {
	zend_mir_test_text_error_code code;
	size_t byte_offset;
	uint32_t line;
	uint32_t column;
	char message[96];
} zend_mir_test_text_error;

bool zend_mir_test_parse_text(const char *text, size_t length,
		zend_mir_fixture_host *host, zend_mir_test_text_error *error);

#endif /* ZEND_MIR_TEST_TEXT_PARSER_H */
