#ifndef ZEND_MIR_VERIFY_CONTROL_FLOW_H
#define ZEND_MIR_VERIFY_CONTROL_FLOW_H

#include "Zend/Native/Lowering/zend_mir_lowering_source.h"
#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/MIR/zend_mir_control_flow.h"

typedef enum _zend_mir_verify_w04_code {
	ZEND_MIR_VERIFY_W04_OK = 0,
	ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH = 600,
	ZEND_MIR_VERIFY_W04_EDGE_MISMATCH = 601,
	ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH = 602,
	ZEND_MIR_VERIFY_W04_PHI_MISMATCH = 603,
	ZEND_MIR_VERIFY_W04_LOOP_MISMATCH = 604,
	ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH = 605,
	ZEND_MIR_VERIFY_W04_CODE_INVALID = -1
} zend_mir_verify_w04_code;

#define ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH "[MIRV0600]"
#define ZEND_MIRV_TOKEN_W04_EDGE_MISMATCH "[MIRV0601]"
#define ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH "[MIRV0602]"
#define ZEND_MIRV_TOKEN_W04_PHI_MISMATCH "[MIRV0603]"
#define ZEND_MIRV_TOKEN_W04_LOOP_MISMATCH "[MIRV0604]"
#define ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH "[MIRV0605]"

bool zend_mir_verify_w04_control_flow(
	const zend_mir_view *view,
	const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	zend_mir_diagnostic_sink *diagnostics);

#endif /* ZEND_MIR_VERIFY_CONTROL_FLOW_H */
