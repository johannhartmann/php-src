/* Private storage shared by the W02-B CFG and PHI implementation. */

#ifndef ZEND_MIR_CFG_INTERNAL_H
#define ZEND_MIR_CFG_INTERNAL_H

#include "zend_mir_cfg.h"

typedef struct _zend_mir_cfg_operand {
	zend_mir_instruction_id instruction_id;
	zend_mir_value_id value_id;
} zend_mir_cfg_operand;

typedef struct _zend_mir_cfg_edge {
	zend_mir_block_id from;
	zend_mir_block_id to;
	uint32_t successor_slot;
	uint32_t predecessor_slot;
} zend_mir_cfg_edge;

struct _zend_mir_cfg {
	zend_mir_allocator allocator;
	zend_mir_diagnostic_sink *diagnostics;
	const zend_mir_view *source;
	zend_mir_view view;
	zend_mir_function_id function_id;
	zend_mir_block_record *blocks;
	uint32_t block_count;
	zend_mir_instruction_record *instructions;
	uint32_t instruction_count;
	zend_mir_cfg_operand *operands;
	uint32_t operand_count;
	zend_mir_cfg_edge *edges;
	uint32_t edge_count;
};

bool zend_mir_cfg_size_multiply(size_t count, size_t width, size_t *out);
void *zend_mir_cfg_allocate(zend_mir_cfg *cfg, size_t count, size_t width,
	size_t alignment, zend_mir_cfg_status *status);
void zend_mir_cfg_emit(zend_mir_cfg *cfg, zend_mir_cfg_status status,
	zend_mir_block_id block_id, zend_mir_instruction_id instruction_id,
	const char *message);
int zend_mir_cfg_find_block(const zend_mir_cfg *cfg, zend_mir_block_id block_id);
int zend_mir_cfg_find_instruction(const zend_mir_cfg *cfg,
	zend_mir_instruction_id instruction_id);
bool zend_mir_cfg_block_is_selected(const zend_mir_cfg *cfg,
	zend_mir_block_id block_id);
uint32_t zend_mir_cfg_predecessor_count_internal(const zend_mir_cfg *cfg,
	zend_mir_block_id block_id);
uint32_t zend_mir_cfg_successor_count_internal(const zend_mir_cfg *cfg,
	zend_mir_block_id block_id);
uint32_t zend_mir_cfg_phi_count_internal(const zend_mir_cfg *cfg,
	zend_mir_block_id block_id);
bool zend_mir_cfg_phi_value_at(const zend_mir_cfg *cfg,
	zend_mir_instruction_id instruction_id, uint32_t slot,
	zend_mir_value_id *value_id);
zend_mir_cfg_status zend_mir_cfg_replace_operands(zend_mir_cfg *cfg,
	const zend_mir_cfg_operand *operands, uint32_t operand_count);
zend_mir_cfg_status zend_mir_cfg_next_block_id(const zend_mir_cfg *cfg,
	zend_mir_block_id *out);
zend_mir_cfg_status zend_mir_cfg_next_instruction_id(const zend_mir_cfg *cfg,
	zend_mir_instruction_id *out);

#endif /* ZEND_MIR_CFG_INTERNAL_H */
