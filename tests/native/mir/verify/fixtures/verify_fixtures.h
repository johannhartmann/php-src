/* Deterministic verifier fixtures built on the frozen W02 fixture host. */

#ifndef ZEND_MIR_VERIFY_FIXTURES_H
#define ZEND_MIR_VERIFY_FIXTURES_H

#include "tests/native/mir/contracts/fixture_host.h"

void zend_mir_verify_fixture_linear(zend_mir_fixture_host *host);
void zend_mir_verify_fixture_diamond(zend_mir_fixture_host *host);

#endif /* ZEND_MIR_VERIFY_FIXTURES_H */
