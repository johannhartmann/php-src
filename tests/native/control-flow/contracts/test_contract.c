#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "Zend/Native/Lowering/zend_mir_control_flow.h"
#include "Zend/Native/Lowering/zend_mir_lowering.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify_control_flow.h"

int main(void)
{
	zend_mir_source_edge_ref edge;
	zend_mir_lowering_result result;

	memset(&edge, 0, sizeof(edge));
	memset(&result, 0, sizeof(result));

	assert(ZEND_MIR_CONTRACT_VERSION == UINT32_C(0x00010002));
	assert(ZEND_MIR_W04_CONTRACT_VERSION == UINT32_C(0x00010003));
	assert(offsetof(zend_mir_source_opcode_ref, block_id)
		> offsetof(zend_mir_source_opcode_ref, source_position_id));
	assert(offsetof(zend_mir_lowering_source_view, block_count)
		> offsetof(zend_mir_lowering_source_view, literal_at));
	assert(offsetof(zend_mir_lowering_source_view, phi_input_at)
		> offsetof(zend_mir_lowering_source_view, phi_count));

	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_FALSE, 0) == 1);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_FALSE, 1) == 0);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_TRUE, 0) == 0);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_TRUE, 1) == 1);

	edge.flags = ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY;
	assert(zend_mir_w04_edge_requires_statepoint(&edge));
	edge.flags = ZEND_MIR_SOURCE_EDGE_BACKEDGE;
	assert(!zend_mir_w04_edge_requires_statepoint(&edge));

	result.status = ZEND_MIR_LOWERING_SUCCESS;
	result.diagnostic_code = ZEND_MIRL_OK;
	result.guarantees = ZEND_MIR_LOWERING_GUARANTEE_W04_ALL;
	result.module = (zend_mir_module *)(uintptr_t) 1;
	assert(zend_mir_lowering_result_is_w04_failure_atomic(&result));
	assert(!zend_mir_lowering_result_is_failure_atomic(&result));

	result.status = ZEND_MIR_LOWERING_FAILED;
	result.diagnostic_code = ZEND_MIRL_STAGE3_VERIFY_FAILED;
	result.guarantees = 0;
	result.module = NULL;
	assert(zend_mir_lowering_result_is_w04_failure_atomic(&result));

	assert(ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH == 600);
	assert(ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH == 605);
	return 0;
}
