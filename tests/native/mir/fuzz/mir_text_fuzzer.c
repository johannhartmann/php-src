/* Optional clang/libFuzzer entry; this file is never linked into production. */

#include "tests/native/mir/text/mir_test_parser.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	zend_mir_fixture_host *host;
	zend_mir_test_text_error error;
	if (size > ZEND_MIR_TEST_TEXT_MAX_BYTES) return 0;
	host = (zend_mir_fixture_host *) malloc(sizeof(*host));
	if (host != NULL) {
		(void) zend_mir_test_parse_text((const char *) data, size, host, &error);
		free(host);
	}
	return 0;
}
