#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.h"

#include <type_traits>

static_assert(std::is_standard_layout_v<zend_mir_scalar_descriptor>);
static_assert(std::is_standard_layout_v<zend_mir_scalar_value_requirement>);

int main()
{
	return zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW) == nullptr;
}
