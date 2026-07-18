#include "Zend/Native/MIR/Verify/zend_mir_verify.h"

#include <type_traits>

static_assert(std::is_enum_v<zend_mir_verify_code>);
static_assert(ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT == 64);

int main()
{
	const zend_mir_view *view = nullptr;
	zend_mir_diagnostic_sink *diagnostics = nullptr;
	(void) zend_mir_verify_stage1(view, diagnostics);
	return zend_mir_verify_code_name(ZEND_MIR_VERIFY_OK) == nullptr;
}
