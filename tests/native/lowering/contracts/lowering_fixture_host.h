#ifndef ZEND_MIR_LOWERING_FIXTURE_HOST_H
#define ZEND_MIR_LOWERING_FIXTURE_HOST_H

#include "../../../../Zend/Native/Lowering/zend_mir_lowering.h"

enum {
	ZEND_MIR_LOWERING_FIXTURE_CAPACITY = 16
};

typedef struct _zend_mir_lowering_fixture_source {
	zend_mir_source_opcode_ref opcodes[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	zend_mir_source_ssa_ref ssa[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	zend_mir_source_ssa_use_ref uses[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	zend_mir_source_ssa_def_ref defs[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	zend_mir_source_literal_ref literals[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	uint32_t opcode_count;
	uint32_t ssa_count;
	uint32_t use_count;
	uint32_t def_count;
	uint32_t literal_count;
} zend_mir_lowering_fixture_source;

typedef struct _zend_mir_lowering_fixture_facts {
	zend_mir_value_fact_ref facts[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	uint32_t count;
} zend_mir_lowering_fixture_facts;

typedef struct _zend_mir_lowering_fixture_registry {
	zend_mir_lowering_provider providers[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	zend_mir_lowering_claim claims[ZEND_MIR_LOWERING_FIXTURE_CAPACITY][ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	uint32_t claim_counts[ZEND_MIR_LOWERING_FIXTURE_CAPACITY];
	uint32_t count;
} zend_mir_lowering_fixture_registry;

void zend_mir_lowering_fixture_source_init(zend_mir_lowering_fixture_source *host,
	zend_mir_lowering_source_view *view);
void zend_mir_lowering_fixture_facts_init(zend_mir_lowering_fixture_facts *host,
	zend_mir_view *view, zend_mir_mutator *mutator);
bool zend_mir_lowering_fixture_add_fact(zend_mir_lowering_fixture_facts *host,
	const zend_mir_value_fact_ref *fact, zend_mir_value_fact_id *out);
bool zend_mir_lowering_fixture_registry_add(zend_mir_lowering_fixture_registry *host,
	const zend_mir_lowering_provider *provider, const zend_mir_lowering_claim *claims,
	uint32_t claim_count, zend_mir_lowering_code *code_out);
bool zend_mir_lowering_fixture_registry_at(const zend_mir_lowering_fixture_registry *host,
	uint32_t index, zend_mir_lowering_provider *out);

#endif /* ZEND_MIR_LOWERING_FIXTURE_HOST_H */
