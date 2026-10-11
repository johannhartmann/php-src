#ifndef ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H
#define ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H

#include "../Core/zend_mir_lowering_internal.h"
#include "../zend_mir_control_flow.h"
#include "../../MIR/ControlFlow/zend_mir_control_flow_internal.h"
#include "../zend_mir_dominator_forest.h"

/* Values are the live zend_vm_opcodes.h opcode numbers. */
enum {
	ZEND_MIR_OPCODE_NOP = 0,
	ZEND_MIR_OPCODE_JMP = 42,
	ZEND_MIR_OPCODE_JMPZ = 43,
	ZEND_MIR_OPCODE_JMPNZ = 44,
	ZEND_MIR_OPCODE_JMPZ_EX = 46,
	ZEND_MIR_OPCODE_JMPNZ_EX = 47,
	ZEND_MIR_OPCODE_CATCH = 107,
	ZEND_MIR_OPCODE_FAST_CALL = 162,
	ZEND_MIR_OPCODE_FAST_RET = 163,
	ZEND_MIR_OPCODE_JMP_SET = 152,
	ZEND_MIR_OPCODE_COALESCE = 169,
	ZEND_MIR_OPCODE_FE_RESET_R = 77,
	ZEND_MIR_OPCODE_FE_FETCH_R = 78,
	ZEND_MIR_OPCODE_FE_RESET_RW = 125,
	ZEND_MIR_OPCODE_FE_FETCH_RW = 126,
	ZEND_MIR_OPCODE_JMP_NULL = 198,
	ZEND_MIR_SOURCE_OPCODE_THROW = 108,
	ZEND_MIR_OPCODE_ASSERT_CHECK = 151,
	ZEND_MIR_OPCODE_SWITCH_LONG = 187,
	ZEND_MIR_OPCODE_SWITCH_STRING = 188,
	ZEND_MIR_OPCODE_MATCH = 195,
	ZEND_MIR_OPCODE_BIND_INIT_STATIC_OR_JMP = 203,
	ZEND_MIR_OPCODE_JMP_FRAMELESS = 208
};

typedef struct _zend_mir_validation {
	uint32_t proofs;
	zend_mir_source_block_id entry_block_id;
	zend_mir_lowering_diagnostic_code diagnostic;
} zend_mir_validation;

/*
 * The block, edge and phi structure of the source view, read once per
 * compile by validation and shared with the phi, cycle and block lowering.
 * Every array lives in one allocation that zend_mir_source_graph_destroy
 * releases.
 */
typedef struct _zend_mir_source_graph {
	uint32_t block_count;
	uint32_t edge_count;
	uint32_t phi_count;
	uint32_t input_count;
	uint32_t ssa_count;
	/* Borrowed: the frontend's forest of the same blocks. */
	const zend_mir_dominator_forest *dominators;
	/* Blocks by id. */
	zend_mir_source_block_ref *blocks;
	/* Outgoing edges of block b, in successor order:
	 * edges[edge_offsets[b] .. edge_offsets[b + 1]). */
	uint32_t *edge_offsets;
	zend_mir_source_edge_ref *edges;
	/* The from block of predecessor slot p of block b:
	 * incoming_from[predecessor_offsets[b] + p], or ZEND_MIR_ID_INVALID. */
	uint32_t *predecessor_offsets;
	uint32_t *incoming_from;
	/* Phis by id; the inputs of phi i, in view order:
	 * inputs[input_offsets[i] .. input_offsets[i + 1]). */
	zend_mir_source_phi_ref *phis;
	uint32_t *input_offsets;
	zend_mir_source_phi_input_ref *inputs;
	/* The phi defining an SSA variable, or ZEND_MIR_ID_INVALID. */
	uint32_t *phi_by_ssa;
	/*
	 * Per input: when its source is a pi of the consuming phi's block with
	 * one input, taken on the same predecessor edge, the pi's own source;
	 * otherwise ZEND_MIR_ID_INVALID. Such a pi lives on that edge only.
	 */
	uint32_t *edge_pi_sources;
	bool protected_region;
	bool irreducible;
	void *storage;
} zend_mir_source_graph;

/*
 * Validates the source view and fills graph, which the caller releases with
 * zend_mir_source_graph_destroy also when validation fails. dominators is
 * the frontend's forest of the view's blocks.
 */
bool zend_mir_validate_source(
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation);
bool zend_mir_validate_source_for_protected_control_flow(
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation);
void zend_mir_source_graph_destroy(zend_mir_source_graph *graph);
zend_mir_branch_kind zend_mir_branch_kind_for_opcode(uint32_t opcode);
bool zend_mir_branch_edge_count_is_valid(
	zend_mir_branch_kind kind, uint32_t opcode, uint32_t edge_count);
bool zend_mir_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	const zend_mir_source_edge_ref *edges,
	uint32_t edge_count,
	bool machine_condition,
	zend_mir_control_flow_map_storage *map);

#endif /* ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H */
