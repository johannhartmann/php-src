#ifndef ZEND_MIR_VERIFICATION_H
#define ZEND_MIR_VERIFICATION_H

#include <stdint.h>

#include "zend_mir.h"

typedef enum _zend_mir_verifier_id {
	ZEND_MIR_VERIFIER_STRUCTURAL = 1,
	ZEND_MIR_VERIFIER_SCALAR = 2,
	ZEND_MIR_VERIFIER_CONTROL_FLOW = 3,
	ZEND_MIR_VERIFIER_CALL_MODEL = 4,
	ZEND_MIR_VERIFIER_ID_INVALID = -1
} zend_mir_verifier_id;

typedef enum _zend_mir_verifier_status {
	ZEND_MIR_VERIFIER_STATUS_PASS = 0,
	ZEND_MIR_VERIFIER_STATUS_FAIL = 1,
	ZEND_MIR_VERIFIER_STATUS_INVALID = -1
} zend_mir_verifier_status;

/*
 * Fingerprints and diagnostic digests are stable 128-bit values represented as
 * four words in network order. Every successful W05 verifier receipt must bind
 * the same final module and source fingerprints.
 */
typedef struct _zend_mir_verifier_receipt_ref {
	zend_mir_verifier_id verifier_id;
	uint32_t verifier_contract_version;
	uint32_t module_fingerprint[4];
	uint32_t source_fingerprint[4];
	zend_mir_verifier_status status;
	uint32_t diagnostic_digest[4];
} zend_mir_verifier_receipt_ref;

#endif /* ZEND_MIR_VERIFICATION_H */
