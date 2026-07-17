/* Predecessor-slot-associated PHI operations for transactional ZNMIR CFGs. */

#ifndef ZEND_MIR_PHI_H
#define ZEND_MIR_PHI_H

#include "zend_mir_cfg.h"

typedef struct _zend_mir_phi_record {
	zend_mir_instruction_id instruction_id;
	zend_mir_block_id block_id;
	zend_mir_value_id result_id;
	zend_mir_representation representation;
	uint32_t incoming_count;
} zend_mir_phi_record;

uint32_t zend_mir_phi_count(const zend_mir_cfg *cfg, zend_mir_block_id block_id);
zend_mir_cfg_status zend_mir_phi_at(const zend_mir_cfg *cfg,
	zend_mir_block_id block_id, uint32_t index, zend_mir_phi_record *out);
zend_mir_cfg_status zend_mir_phi_incoming_at(const zend_mir_cfg *cfg,
	zend_mir_instruction_id phi_instruction_id, uint32_t predecessor_slot,
	zend_mir_block_id *predecessor_id, zend_mir_value_id *value_id);
zend_mir_cfg_status zend_mir_phi_set_incoming(zend_mir_cfg *cfg,
	zend_mir_instruction_id phi_instruction_id, zend_mir_block_id predecessor_id,
	zend_mir_value_id value_id);

#endif /* ZEND_MIR_PHI_H */
