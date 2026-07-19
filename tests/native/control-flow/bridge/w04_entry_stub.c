/*
 * Branch-local link stub for W04-B compile and schema tests.
 *
 * This file is test evidence only. It is intentionally absent from config.m4
 * and cannot publish a module, guarantees, or control-flow semantics.
 */
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

zend_mir_lowering_result zend_mir_lower_w04_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *map)
{
	zend_mir_lowering_result result;

	(void) context;
	(void) mutator;
	(void) map;
	result.status = ZEND_MIR_LOWERING_DEFERRED;
	result.diagnostic_code = ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED;
	result.guarantees = 0;
	result.module = NULL;
	return result;
}
