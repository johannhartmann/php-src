#ifndef ZEND_MIR_LOWERING_REGISTRY_H
#define ZEND_MIR_LOWERING_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "../MIR/zend_mir.h"
#include "zend_mir_lowering_diagnostic.h"
#include "zend_mir_lowering_source.h"

typedef struct _zend_mir_lowering_context zend_mir_lowering_context;
typedef struct _zend_mir_lowering_registry zend_mir_lowering_registry;

typedef struct _zend_mir_lowering_claim {
	uint32_t zend_opcode_number;
	uint32_t semantic_family_id;
} zend_mir_lowering_claim;

typedef zend_mir_lowering_status (*zend_mir_lower_opcode_fn)(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator);

typedef struct _zend_mir_lowering_provider {
	uint32_t provider_id;
	uint32_t semantic_family_id;
	const void *context;
	uint32_t (*claim_count)(const void *context);
	bool (*claim_at)(const void *context, uint32_t index, zend_mir_lowering_claim *out);
	zend_mir_lower_opcode_fn lower;
} zend_mir_lowering_provider;

/*
 * Registration rejects duplicate provider IDs and overlapping opcode claims.
 * Enumeration is ascending by (semantic_family_id, provider_id), independent
 * of insertion order.
 */
bool zend_mir_lowering_registry_register(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_provider *provider,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
uint32_t zend_mir_lowering_registry_provider_count(const zend_mir_lowering_registry *registry);
bool zend_mir_lowering_registry_provider_at(const zend_mir_lowering_registry *registry,
	uint32_t index, zend_mir_lowering_provider *out);

#endif /* ZEND_MIR_LOWERING_REGISTRY_H */
