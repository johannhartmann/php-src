#include "Zend/Native/Lowering/Scalar/Numeric/zend_mir_lower_numeric.h"

static_assert(ZEND_MIR_NUMERIC_PROVIDER_COUNT == 3);

int numeric_header_is_cxx20_compatible()
{
	return ZEND_MIR_NUMERIC_FAMILY_ID == 3 ? 0 : 1;
}
