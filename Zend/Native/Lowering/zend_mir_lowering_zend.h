#ifndef ZEND_MIR_LOWERING_ZEND_H
#define ZEND_MIR_LOWERING_ZEND_H

#include "../MIR/zend_mir_control_flow.h"
#include "zend_mir_lowering.h"

/*
 * Internal W04 entry point. The source view owned by context and the
 * process-local map must remain alive through stage-3 verification.
 */
zend_mir_lowering_result zend_mir_lower_w04_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *map);

#endif /* ZEND_MIR_LOWERING_ZEND_H */
