#include "fixture_host.h"

#include <assert.h>
#include <string.h>

static bool count_diagnostic(void *context, const zend_mir_diagnostic *diagnostic)
{
	uint32_t *count = (uint32_t *) context;

	assert(diagnostic->code == ZEND_MIR_DIAGNOSTIC_INVALID_ID);
	(*count)++;
	return true;
}

int main(void)
{
	zend_mir_fixture_host host;
	zend_mir_function_id function_id;
	zend_mir_block_id entry_id;
	zend_mir_block_id exit_id;
	zend_mir_block_id false_id;
	zend_mir_block_id merge_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_instruction_id phi_instruction_id;
	zend_mir_instruction_record instruction;
	zend_mir_instruction_record observed;
	zend_mir_block_id observed_block;
	zend_mir_value_id value_id = zend_mir_value_from_original_ssa(7);
	zend_mir_value_id false_value_id = zend_mir_value_from_original_ssa(8);
	zend_mir_value_id phi_value_id = zend_mir_value_from_synthetic(0);
	zend_mir_value_id observed_value;
	zend_mir_constant_record constant;
	zend_mir_constant_record observed_constant;
	zend_mir_source_position_ref source_position;
	zend_mir_source_position_ref observed_position;
	zend_mir_source_position_id source_position_id;
	zend_mir_frame_slot_ref slot;
	zend_mir_frame_slot_ref observed_slot;
	zend_mir_cleanup_ref cleanup;
	zend_mir_cleanup_ref observed_cleanup;
	zend_mir_frame_state_ref frame_state;
	zend_mir_frame_state_ref observed_frame_state;
	zend_mir_frame_state_id frame_state_id;
	uint32_t slot_index;
	uint32_t root_index;
	uint32_t cleanup_index;
	uint32_t observed_slot_id;
	uint32_t diagnostic_count = 0;
	zend_mir_diagnostic diagnostic;
	zend_mir_diagnostic_sink sink;
	zend_mir_instruction_id empty_action_instruction_id;

	memset(&instruction, 0, sizeof(instruction));
	memset(&source_position, 0, sizeof(source_position));
	memset(&constant, 0, sizeof(constant));
	memset(&slot, 0, sizeof(slot));
	memset(&cleanup, 0, sizeof(cleanup));
	memset(&frame_state, 0, sizeof(frame_state));
	memset(&diagnostic, 0, sizeof(diagnostic));
	memset(&sink, 0, sizeof(sink));
	zend_mir_fixture_host_init(&host, 19);
	assert(zend_mir_contract_is_compatible(host.view.contract_version));
	assert(zend_mir_value_is_original_ssa(ZEND_MIR_VALUE_ORIGINAL_MAX));
	assert(zend_mir_value_from_synthetic(ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX)
		== ZEND_MIR_VALUE_SYNTHETIC_MAX);
	assert(zend_mir_value_from_synthetic(ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX + 1)
		== ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_function(host.mutator.context, 11, &function_id));
	assert(host.mutator.add_block(host.mutator.context, function_id, &entry_id));
	assert(host.mutator.add_block(host.mutator.context, function_id, &exit_id));
	assert(host.mutator.add_block(host.mutator.context, function_id, &false_id));
	assert(host.mutator.add_block(host.mutator.context, function_id, &merge_id));
	assert(host.mutator.set_entry_block(host.mutator.context, function_id, entry_id));
	assert(host.mutator.add_value(host.mutator.context, value_id,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host.mutator.add_value(host.mutator.context, false_value_id,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	assert(host.mutator.add_value(host.mutator.context, phi_value_id,
		ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	constant.value_id = value_id;
	constant.representation = ZEND_MIR_REPRESENTATION_ZVAL;
	constant.kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	constant.payload_bits = UINT64_C(42);
	constant.symbol_id = ZEND_MIR_ID_INVALID;
	assert(host.mutator.add_constant(host.mutator.context, &constant));
	assert(!host.mutator.add_constant(host.mutator.context, &constant));

	source_position.file_symbol_id = 21;
	source_position.line = 4;
	source_position.column_start = 2;
	source_position.column_end = 8;
	assert(host.mutator.add_source_position(
		host.mutator.context, &source_position, &source_position_id));
	slot.slot_id = 23;
	slot.value_id = value_id;
	slot.index = 0;
	slot.kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	slot.representation = ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED;
	slot.rooted = true;
	slot.cleanup_required = true;
	assert(host.mutator.add_frame_slot(host.mutator.context, &slot, &slot_index));
	assert(host.mutator.add_root(host.mutator.context, slot.slot_id, &root_index));
	cleanup.slot_id = slot.slot_id;
	cleanup.action = ZEND_MIR_CLEANUP_ACTION_RELEASE;
	cleanup.state = ZEND_MIR_CLEANUP_STATE_PENDING;
	assert(host.mutator.add_cleanup(host.mutator.context, &cleanup, &cleanup_index));
	frame_state.function_id = function_id;
	frame_state.parent_id = ZEND_MIR_ID_INVALID;
	frame_state.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame_state.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame_state.slots.offset = slot_index;
	frame_state.slots.count = 1;
	frame_state.roots.offset = root_index;
	frame_state.roots.count = 1;
	frame_state.cleanup_obligations.offset = cleanup_index;
	frame_state.cleanup_obligations.count = 1;
	frame_state.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame_state.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
	frame_state.canonical = true;
	assert(host.mutator.add_frame_state(host.mutator.context, &frame_state, &frame_state_id));

	instruction.block_id = entry_id;
	instruction.opcode = ZEND_MIR_OPCODE_COND_BRANCH;
	instruction.representation = ZEND_MIR_REPRESENTATION_CONTROL;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = frame_state_id;
	instruction.source_position_id = source_position_id;
	instruction.ownership_actions = ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_BORROW)
		| ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_CANONICALIZE);
	assert(host.mutator.add_instruction(host.mutator.context, &instruction, &instruction_id));
	assert(host.mutator.add_operand(host.mutator.context, instruction_id, value_id));
	assert(host.mutator.add_edge(host.mutator.context, entry_id, exit_id));
	assert(host.mutator.add_edge(host.mutator.context, entry_id, false_id));
	assert(host.mutator.add_edge(host.mutator.context, exit_id, merge_id));
	assert(host.mutator.add_edge(host.mutator.context, false_id, merge_id));
	assert(!host.mutator.add_edge(host.mutator.context, entry_id, exit_id));
	memset(&instruction, 0, sizeof(instruction));
	instruction.block_id = exit_id;
	instruction.opcode = ZEND_MIR_OPCODE_RETURN;
	instruction.representation = ZEND_MIR_REPRESENTATION_CONTROL;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = frame_state_id;
	instruction.source_position_id = source_position_id;
	assert(host.mutator.add_instruction(
		host.mutator.context, &instruction, &empty_action_instruction_id));
	memset(&instruction, 0, sizeof(instruction));
	instruction.block_id = merge_id;
	instruction.opcode = ZEND_MIR_OPCODE_PHI;
	instruction.representation = ZEND_MIR_REPRESENTATION_ZVAL;
	instruction.result_id = phi_value_id;
	instruction.frame_state_id = frame_state_id;
	instruction.source_position_id = source_position_id;
	assert(host.mutator.add_instruction(host.mutator.context, &instruction, &phi_instruction_id));
	assert(host.mutator.add_operand(host.mutator.context, phi_instruction_id, value_id));
	assert(host.mutator.add_operand(host.mutator.context, phi_instruction_id, false_value_id));
	assert(host.mutator.seal_function(host.mutator.context, function_id));

	assert(host.view.module_id(host.view.context) == 19);
	assert(host.view.function_count(host.view.context) == 1);
	assert(host.view.block_count(host.view.context) == 4);
	assert(host.view.instruction_count(host.view.context) == 3);
	assert(host.view.instruction_at(host.view.context, 0, &observed));
	assert(observed.id == instruction_id);
	assert(observed.ownership_actions
		== (ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_BORROW)
			| ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_CANONICALIZE)));
	assert(host.view.instruction_at(host.view.context, 1, &observed));
	assert(observed.id == empty_action_instruction_id);
	assert(observed.ownership_actions == 0);
	assert(host.view.constant_count(host.view.context) == 1);
	assert(host.view.constant_at(host.view.context, 0, &observed_constant));
	assert(observed_constant.payload_bits == UINT64_C(42));
	assert(host.view.instruction_operand_count(host.view.context, instruction_id) == 1);
	assert(host.view.successor_count(host.view.context, entry_id) == 2);
	assert(host.view.successor_at(host.view.context, entry_id, 0, &observed_block));
	assert(observed_block == exit_id);
	assert(host.view.successor_at(host.view.context, entry_id, 1, &observed_block));
	assert(observed_block == false_id);
	assert(host.view.predecessor_count(host.view.context, exit_id) == 1);
	assert(host.view.predecessor_at(host.view.context, exit_id, 0, &observed_block));
	assert(observed_block == entry_id);
	assert(host.view.predecessor_count(host.view.context, merge_id) == 2);
	assert(host.view.predecessor_at(host.view.context, merge_id, 0, &observed_block));
	assert(observed_block == exit_id);
	assert(host.view.instruction_operand_at(
		host.view.context, phi_instruction_id, 0, &observed_value));
	assert(observed_value == value_id);
	assert(host.view.predecessor_at(host.view.context, merge_id, 1, &observed_block));
	assert(observed_block == false_id);
	assert(host.view.instruction_operand_at(
		host.view.context, phi_instruction_id, 1, &observed_value));
	assert(observed_value == false_value_id);
	assert(host.view.source_position_count(host.view.context) == 1);
	assert(host.view.source_position_at(host.view.context, 0, &observed_position));
	assert(observed_position.id == source_position_id);
	assert(host.view.frame_slot_count(host.view.context) == 1);
	assert(host.view.frame_slot_at(host.view.context, slot_index, &observed_slot));
	assert(observed_slot.slot_id == slot.slot_id);
	assert(host.view.root_at(host.view.context, root_index, &observed_slot_id));
	assert(observed_slot_id == slot.slot_id);
	assert(host.view.cleanup_at(host.view.context, cleanup_index, &observed_cleanup));
	assert(observed_cleanup.slot_id == slot.slot_id);
	assert(host.view.frame_state_at(host.view.context, frame_state_id, &observed_frame_state));
	assert(observed_frame_state.slots.count == 1);

	diagnostic.code = ZEND_MIR_DIAGNOSTIC_INVALID_ID;
	sink.context = &diagnostic_count;
	sink.emit = count_diagnostic;
	sink.limit = 1;
	assert(zend_mir_diagnostic_sink_emit(&sink, &diagnostic));
	assert(!zend_mir_diagnostic_sink_emit(&sink, &diagnostic));
	assert(diagnostic_count == 1);

	return 0;
}
