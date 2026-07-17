/* Transactional, target-neutral ZNMIR control-flow graph operations. */

#ifndef ZEND_MIR_CFG_H
#define ZEND_MIR_CFG_H

#include "Zend/Native/MIR/zend_mir.h"

typedef enum _zend_mir_cfg_status {
	ZEND_MIR_CFG_STATUS_OK = 0,
	ZEND_MIR_CFG_STATUS_INVALID_ARGUMENT = 1,
	ZEND_MIR_CFG_STATUS_INCOMPATIBLE_CONTRACT = 2,
	ZEND_MIR_CFG_STATUS_NOT_FOUND = 3,
	ZEND_MIR_CFG_STATUS_DUPLICATE_EDGE = 4,
	ZEND_MIR_CFG_STATUS_INVALID_CFG = 5,
	ZEND_MIR_CFG_STATUS_INVALID_PHI = 6,
	ZEND_MIR_CFG_STATUS_ALLOCATION_FAILED = 7,
	ZEND_MIR_CFG_STATUS_CAPACITY_EXCEEDED = 8
} zend_mir_cfg_status;

typedef struct _zend_mir_cfg zend_mir_cfg;

/* One value for a PHI in the new destination of add/retarget. */
typedef struct _zend_mir_cfg_phi_incoming {
	zend_mir_instruction_id phi_instruction_id;
	zend_mir_value_id value_id;
} zend_mir_cfg_phi_incoming;

/*
 * The allocator is owned by the CFG until destroy and must not be shared with
 * another live arena owner. The source view must outlive the CFG and remain
 * unchanged. Mutations are published through the derived view only after
 * every allocation and precondition has succeeded.
 */
zend_mir_cfg_status zend_mir_cfg_create(zend_mir_cfg **out,
	const zend_mir_view *source, zend_mir_function_id function_id,
	zend_mir_allocator allocator, zend_mir_diagnostic_sink *diagnostics);
void zend_mir_cfg_destroy(zend_mir_cfg *cfg);

const zend_mir_view *zend_mir_cfg_view(const zend_mir_cfg *cfg);
zend_mir_function_id zend_mir_cfg_function_id(const zend_mir_cfg *cfg);
const char *zend_mir_cfg_status_name(zend_mir_cfg_status status);

zend_mir_cfg_status zend_mir_cfg_validate(const zend_mir_cfg *cfg);
zend_mir_cfg_status zend_mir_cfg_add_edge(zend_mir_cfg *cfg,
	zend_mir_block_id from, zend_mir_block_id to,
	const zend_mir_cfg_phi_incoming *incoming, uint32_t incoming_count);
zend_mir_cfg_status zend_mir_cfg_remove_edge(zend_mir_cfg *cfg,
	zend_mir_block_id from, zend_mir_block_id to);
zend_mir_cfg_status zend_mir_cfg_retarget_edge(zend_mir_cfg *cfg,
	zend_mir_block_id from, zend_mir_block_id old_to, zend_mir_block_id new_to,
	const zend_mir_cfg_phi_incoming *incoming, uint32_t incoming_count);

/* Split before instruction_id. The old block retains its PHIs and entry ID. */
zend_mir_cfg_status zend_mir_cfg_split_block(zend_mir_cfg *cfg,
	zend_mir_block_id block_id, zend_mir_instruction_id instruction_id,
	zend_mir_block_id *new_block_id);

/* Replace from->to by from->synthetic->to without changing destination PHIs. */
zend_mir_cfg_status zend_mir_cfg_split_edge(zend_mir_cfg *cfg,
	zend_mir_block_id from, zend_mir_block_id to,
	zend_mir_block_id *new_block_id);

#endif /* ZEND_MIR_CFG_H */
