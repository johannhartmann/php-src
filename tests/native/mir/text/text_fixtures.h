#ifndef ZEND_MIR_TEST_TEXT_FIXTURES_H
#define ZEND_MIR_TEST_TEXT_FIXTURES_H

#include "tests/native/mir/contracts/fixture_host.h"

#define ZEND_MIR_TEXT_FIXTURE_COUNT 6U

extern const char *const zend_mir_text_fixture_names[ZEND_MIR_TEXT_FIXTURE_COUNT];

bool zend_mir_text_build_fixture(const char *name, zend_mir_fixture_host *host);

#endif /* ZEND_MIR_TEST_TEXT_FIXTURES_H */
