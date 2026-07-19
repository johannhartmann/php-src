#ifndef ZEND_MIR_CALL_MODEL_H
#define ZEND_MIR_CALL_MODEL_H

#include "../../Lowering/zend_mir_lowering_zend.h"

zend_mir_w05_lowering_result zend_mir_lower_w05_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

#endif /* ZEND_MIR_CALL_MODEL_H */
