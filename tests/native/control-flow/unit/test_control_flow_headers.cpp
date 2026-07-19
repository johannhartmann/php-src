#include <type_traits>

#include "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_internal.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_w04.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/MIR/ControlFlow/zend_mir_control_flow_internal.h"

static_assert(std::is_standard_layout_v<zend_mir_w04_validation>);
static_assert(std::is_standard_layout_v<zend_mir_control_flow_map_storage>);
static_assert(std::is_trivially_copyable_v<zend_mir_source_edge_ref>);
static_assert(std::is_trivially_copyable_v<zend_mir_source_phi_ref>);

int main()
{
	return 0;
}
