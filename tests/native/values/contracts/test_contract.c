#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W06_CONTRACT_VERSION_MINOR == 9,
	"W06 contract version");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_STORAGE_BIND == 42,
	"W06 opcode base");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W06_OPCODE_COUNT == 48,
	"W06 opcode boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH == 806,
	"W06 call-transfer verifier diagnostic");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_STORAGE_REFERENCE != ZEND_MIR_STORAGE_INDIRECT,
	"reference and indirect storage remain distinct");

int main(void)
{
	zend_mir_parameter_mode_ref mode = {
		UINT32_C(128), UINT32_C(128), ZEND_MIR_PARAMETER_BY_REFERENCE
	};
	zend_mir_storage_ref indirect = {
		UINT32_C(1),
		ZEND_MIR_STORAGE_INDIRECT_SLOT,
		ZEND_MIR_STORAGE_INDIRECT,
		ZEND_MIR_VALUE_CATEGORY_UNKNOWN,
		ZEND_MIR_ID_INVALID,
		ZEND_MIR_ID_INVALID,
		UINT32_C(2)
	};
	return mode.ordinal == UINT32_C(128)
		&& indirect.state != ZEND_MIR_STORAGE_REFERENCE ? 0 : 1;
}
