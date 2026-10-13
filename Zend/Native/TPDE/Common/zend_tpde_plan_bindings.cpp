// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

struct zend_tpde_source_definition_index {
	const uint32_t *offsets;
	const uint32_t *instructions;
	const uint32_t *direct_offsets;
	const uint32_t *direct_instructions;
	uint32_t storage_count;
};

static zend_tpde_source_value_binding freeze_source_value_binding(
	const zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_source_operand_ref &operand,
	zend_mir_storage_id storage_id,
	uint32_t consumer_source_position,
	uint32_t consumer_instruction_index,
	const zend_tpde_source_definition_index &definitions)
{
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	int32_t value_index = -1;
	if (source_operand_value_id(operand, value_id)) {
		value_index = zend_tpde_value_index(plan, value_id);
	}
	if (value_index < 0
			&& (operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& operand.index < plan->argument_count
			&& plan->argument_value_indices != nullptr
			&& plan->argument_value_indices[operand.index] >= 0) {
		value_index = plan->argument_value_indices[operand.index];
	}
	if (!zend_mir_id_is_valid(storage_id)) {
		return {value_index, -1};
	}
	if (storage_id >= definitions.storage_count) {
		return {value_index, -1};
	}

	int32_t definition = -1;
	uint32_t definition_source_position = 0;
	const bool call_argument = consumer_instruction_index == UINT32_MAX;
	const bool operand_has_ssa =
		zend_mir_id_is_valid(operand.ssa_variable_id);
	const uint32_t *definition_offsets = call_argument
		? definitions.offsets : definitions.direct_offsets;
	const uint32_t *definition_instructions = call_argument
		? definitions.instructions : definitions.direct_instructions;
	const uint32_t definition_begin = definition_offsets[storage_id];
	const uint32_t definition_end = definition_offsets[storage_id + 1];
	for (uint32_t definition_offset = definition_begin;
			definition_offset < definition_end; ++definition_offset) {
		const uint32_t index = definition_instructions[definition_offset];
		const zend_tpde_instruction &candidate = plan->instructions[index];
		const zend_mir_instruction_record candidate_record =
			zend_tpde_instruction_record_at(plan, &candidate);
		zend_mir_source_operand_ref candidate_result{};
		zend_mir_storage_id candidate_storage = ZEND_MIR_ID_INVALID;
		uint32_t candidate_source_position = UINT32_MAX;
		bool has_candidate_result = false;

		if (candidate.direct_call != nullptr) {
			candidate_result = candidate.direct_call->result_operand;
			candidate_storage = source_descriptor_storage(
				source_op_array, candidate_result);
			candidate_source_position =
				candidate.call_site->source_do_opline_index;
			has_candidate_result = true;
		} else if (call_argument
				&& candidate.direct_internal_call != nullptr) {
			candidate_result = candidate.direct_internal_call->result_operand;
			candidate_storage = source_descriptor_storage(
				source_op_array, candidate_result);
			candidate_source_position =
				candidate.call_site->source_do_opline_index;
			has_candidate_result = true;
		} else if (call_argument && candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			candidate_result = candidate.value_operation.result;
			candidate_storage =
				candidate.value_operation.result_storage_id;
			candidate_source_position = candidate_record.source_position_id;
			has_candidate_result = true;
		}
		if (!has_candidate_result
				|| index >= consumer_instruction_index
				|| candidate_source_position > consumer_source_position
				|| candidate_storage != storage_id
				|| (call_argument && operand_has_ssa
					&& (!zend_mir_id_is_valid(
							candidate_result.ssa_variable_id)
						|| operand.ssa_variable_id
							!= candidate_result.ssa_variable_id))
				|| (!call_argument && operand_has_ssa
					&& zend_mir_id_is_valid(
						candidate_result.ssa_variable_id)
					&& operand.ssa_variable_id
						!= candidate_result.ssa_variable_id)
				|| (call_argument && !operand_has_ssa
					&& (consumer_source_position
							>= plan->source_opcode_count
						|| candidate_source_position
							>= plan->source_opcode_count
						|| plan->source_opcode_block_indices == nullptr
						|| plan->source_opcode_block_indices[
							consumer_source_position]
							!= plan->source_opcode_block_indices[
								candidate_source_position]))) {
			continue;
		}
		if (definition < 0
				|| candidate_source_position >= definition_source_position) {
			definition = static_cast<int32_t>(index);
			definition_source_position = candidate_source_position;
		}
	}
	return {value_index, definition};
}

static int32_t freeze_reaching_phi_value(
	const zend_tpde_plan *plan,
	zend_mir_block_id consumer_block_id,
	zend_mir_storage_id storage_id)
{
	if (!zend_mir_id_is_valid(consumer_block_id)
			|| !zend_mir_id_is_valid(storage_id)
			|| plan->block_predecessor_offsets == nullptr) {
		return -1;
	}
	const int32_t consumer_block =
		zend_tpde_block_index(plan, consumer_block_id);
	if (consumer_block < 0) {
		return -1;
	}
	/* The first PHI of each block that defines the storage. */
	std::vector<int32_t> block_phi(plan->block_count, -1);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_mir_instruction_record &candidate_record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[index]);
		if (candidate_record.opcode != ZEND_MIR_OPCODE_PHI
				|| !zend_mir_id_is_valid(candidate_record.result_id)) {
			continue;
		}
		const int32_t block =
			zend_tpde_block_index(plan, candidate_record.block_id);
		const int32_t value =
			zend_tpde_value_index(plan, candidate_record.result_id);
		if (block >= 0 && block_phi[static_cast<uint32_t>(block)] < 0
				&& value >= 0
				&& plan->values[value].canonical_storage_id == storage_id) {
			block_phi[static_cast<uint32_t>(block)] = value;
		}
	}
	/*
	 * The walk stops at such a PHI and cuts cycles at blocks it is still
	 * visiting, which makes a block's answer depend on the path to it. Without
	 * a cycle among the blocks it expands, no cut occurs and the answer is a
	 * function of the block, which may then be reused instead of walking each
	 * path through nested diamonds again.
	 */
	std::vector<uint8_t> state(plan->block_count, 0);
	bool cyclic = false;
	{
		std::vector<std::pair<uint32_t, uint32_t>> stack;
		stack.emplace_back(static_cast<uint32_t>(consumer_block), 0);
		state[static_cast<uint32_t>(consumer_block)] = 1;
		while (!stack.empty() && !cyclic) {
			auto &[block, edge] = stack.back();
			const uint32_t begin = plan->block_predecessor_offsets[block];
			const uint32_t end = plan->block_predecessor_offsets[block + 1];
			if (block_phi[block] >= 0 || begin + edge >= end) {
				state[block] = 2;
				stack.pop_back();
				continue;
			}
			const uint32_t predecessor =
				plan->block_predecessors[begin + edge++];
			if (predecessor >= plan->block_count
					|| state[predecessor] == 2) {
				continue;
			}
			if (state[predecessor] == 1) {
				cyclic = true;
				break;
			}
			state[predecessor] = 1;
			stack.emplace_back(predecessor, 0);
		}
	}
	constexpr int32_t unresolved = INT32_MIN;
	std::vector<int32_t> resolved(cyclic ? 0 : plan->block_count, unresolved);
	std::vector<uint8_t> visiting(plan->block_count, 0);
	auto resolve = [&](uint32_t block_index, auto &&self) -> int32_t {
		if (block_index >= plan->block_count || visiting[block_index] != 0) {
			return -1;
		}
		if (block_phi[block_index] >= 0) {
			return block_phi[block_index];
		}
		if (!cyclic && resolved[block_index] != unresolved) {
			return resolved[block_index];
		}
		visiting[block_index] = 1;
		const uint32_t begin = plan->block_predecessor_offsets[block_index];
		const uint32_t end = plan->block_predecessor_offsets[block_index + 1];
		int32_t merged = -1;
		for (uint32_t edge = begin; edge < end; ++edge) {
			const int32_t incoming =
				self(plan->block_predecessors[edge], self);
			if (incoming < 0) {
				continue;
			}
			if (merged >= 0 && merged != incoming) {
				merged = -1;
				break;
			}
			merged = incoming;
		}
		visiting[block_index] = 0;
		if (!cyclic) {
			resolved[block_index] = merged;
		}
		return merged;
	};
	return resolve(static_cast<uint32_t>(consumer_block), resolve);
}

bool freeze_source_value_bindings(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag)
{
	if (source_op_array == nullptr) {
		return true;
	}
	const uint32_t storage_count =
		static_cast<uint32_t>(source_op_array->last_var)
		+ source_op_array->T;
	std::vector<uint32_t> definition_offsets(
		static_cast<size_t>(storage_count) + 1, 0);
	std::vector<uint32_t> direct_definition_offsets(
		static_cast<size_t>(storage_count) + 1, 0);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &candidate = plan->instructions[index];
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;

		if (candidate.direct_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array, candidate.direct_call->result_operand);
		} else if (candidate.direct_internal_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array,
				candidate.direct_internal_call->result_operand);
		} else if (candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			storage_id = candidate.value_operation.result_storage_id;
		}
		if (storage_id < storage_count) {
			definition_offsets[storage_id + 1]++;
			if (candidate.direct_call != nullptr) {
				direct_definition_offsets[storage_id + 1]++;
			}
		}
	}
	for (uint32_t storage = 0; storage < storage_count; ++storage) {
		definition_offsets[storage + 1] += definition_offsets[storage];
		direct_definition_offsets[storage + 1] +=
			direct_definition_offsets[storage];
	}
	std::vector<uint32_t> definition_instructions(
		definition_offsets[storage_count]);
	std::vector<uint32_t> direct_definition_instructions(
		direct_definition_offsets[storage_count]);
	std::vector<uint32_t> definition_cursors = definition_offsets;
	std::vector<uint32_t> direct_definition_cursors =
		direct_definition_offsets;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &candidate = plan->instructions[index];
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;

		if (candidate.direct_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array, candidate.direct_call->result_operand);
		} else if (candidate.direct_internal_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array,
				candidate.direct_internal_call->result_operand);
		} else if (candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			storage_id = candidate.value_operation.result_storage_id;
		}
		if (storage_id < storage_count) {
			definition_instructions[definition_cursors[storage_id]++] = index;
			if (candidate.direct_call != nullptr) {
				direct_definition_instructions[
					direct_definition_cursors[storage_id]++] = index;
			}
		}
	}
	const zend_tpde_source_definition_index definitions = {
		definition_offsets.data(), definition_instructions.data(),
		direct_definition_offsets.data(), direct_definition_instructions.data(),
		storage_count};
	for (uint32_t index = 0; index < plan->call_argument_count; ++index) {
		zend_mir_call_argument_ref argument{};
		if (!zend_tpde_call_argument_at(plan, index, &argument)
				|| argument.send_opline_index
					>= source_op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze a source call-argument value");
			return false;
		}
		plan->call_argument_bindings[index] =
			freeze_source_value_binding(
				plan, source_op_array, argument.source_operand,
				source_descriptor_storage(
					source_op_array, argument.source_operand),
				argument.send_opline_index, UINT32_MAX, definitions);
	}

	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!zend_mir_id_is_valid(record.id)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze source values for an unknown instruction");
			return false;
		}
		const uint32_t source_position =
			zend_mir_id_is_valid(record.source_position_id)
				? record.source_position_id : UINT32_MAX;
		if (instruction.has_value_operation) {
			const zend_mir_executable_value_ref &operation =
				instruction.value_operation;
			instruction.source_op1_binding = freeze_source_value_binding(
				plan, source_op_array, operation.op1,
				operation.op1_storage_id,
				source_position, index, definitions);
			instruction.source_op2_binding = freeze_source_value_binding(
				plan, source_op_array, operation.op2,
				operation.op2_storage_id,
				source_position, index, definitions);
			/* Without SSA facts a CV operand may hold a reference. */
			instruction.source_op1_may_reference = true;
			instruction.source_op2_may_reference = true;
			if (source_ssa != nullptr && source_ssa->var_info != nullptr
					&& source_position < source_op_array->last) {
				auto reference_free = [&](int use) {
					return source_ssa->vars[use].alias == NO_ALIAS
						&& !zend_tpde_ssa_variable_rebindable(
							source_op_array, source_ssa,
							static_cast<uint32_t>(use))
						&& (source_ssa->var_info[use].type & MAY_BE_REF) == 0;
				};
				const int op1_use = source_ssa->ops[source_position].op1_use;
				const int op2_use = source_ssa->ops[source_position].op2_use;
				const bool op1_known =
					op1_use >= 0 && op1_use < source_ssa->vars_count;
				const bool op2_known =
					op2_use >= 0 && op2_use < source_ssa->vars_count;
				instruction.source_op1_reference_free =
					op1_known && reference_free(op1_use);
				instruction.source_op1_may_reference =
					!op1_known || !reference_free(op1_use);
				instruction.source_op2_may_reference =
					!op2_known || !reference_free(op2_use);
			}
			if (source_ssa != nullptr && source_ssa->var_info != nullptr
					&& source_position < source_op_array->last
					&& source_ssa->ops[source_position].op2_def >= 0) {
				const zend_ssa_op &source_op =
					source_ssa->ops[source_position];
				const uint32_t definition = static_cast<uint32_t>(
					source_op.op2_def);
				if (definition >= static_cast<uint32_t>(source_ssa->vars_count)
						|| source_ssa->vars[definition].var < 0
						|| static_cast<uint32_t>(
							source_ssa->vars[definition].var)
							!= operation.op2_storage_id) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source op2 definition does not match its storage");
					return false;
				}
				const int32_t value_index = zend_tpde_value_index(
					plan, zend_mir_value_from_original_ssa(definition));
				if (value_index >= 0) {
					instruction.source_op2_definition_binding =
						{value_index, -1};
					instruction
						.source_op2_definition_ssa_variable_id_plus_one =
						definition + 1;
					if (source_op.op2_use >= 0
							&& source_op.op2_use < source_ssa->vars_count
							&& source_ssa->vars[source_op.op2_use].alias
								== NO_ALIAS) {
						const uint32_t use_type =
							source_ssa->var_info[source_op.op2_use].type
								& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF);
						const uint32_t definition_type =
							source_ssa->var_info[definition].type
								& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF);
						/*
						 * An unaliased destination whose reaching value is only
						 * UNDEF or LONG needs no reference resolution and no
						 * destructor, so the packed LONG iterator path skips its
						 * type guard.
						 */
						instruction.source_op2_canonical_scalar_only =
							use_type != 0
							&& (use_type
								& ~(MAY_BE_UNDEF | MAY_BE_LONG)) == 0
							&& definition_type == MAY_BE_LONG;
					}
				}
			}
			instruction.source_result_binding = freeze_source_value_binding(
				plan, source_op_array, operation.result,
				operation.result_storage_id,
				source_position, index, definitions);
			instruction.source_auxiliary_binding =
				freeze_source_value_binding(
					plan, source_op_array, operation.auxiliary,
					operation.auxiliary_storage_id, source_position,
					index, definitions);
			/*
			 * Zend SSA omits the RETURN use when a preceding VERIFY_RETURN_TYPE
			 * consumed the same slot. Take the MIR edge from that check, not a
			 * reaching definition from the slot's linear history, which is wrong
			 * at loop exits: a backedge call does not dominate the return block.
			 */
			if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					&& instruction.source_op1_binding.value_index < 0
					&& !zend_mir_id_is_valid(
						operation.op1.ssa_variable_id)
					&& zend_mir_id_is_valid(operation.op1_storage_id)) {
				for (uint32_t previous = index; previous != 0; --previous) {
					const zend_tpde_instruction &candidate =
						plan->instructions[previous - 1];
					const zend_mir_instruction_record candidate_record =
						zend_tpde_instruction_record_at(plan, &candidate);
					if (candidate_record.block_id != record.block_id) {
						break;
					}
					if (candidate_record.opcode
							!= ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
							|| candidate.operand_count != 1) {
						continue;
					}
					const int32_t verified = zend_tpde_value_index(
						plan, zend_tpde_operand_at(plan, &candidate, 0));
					if (verified >= 0
							&& plan->values[verified].canonical_storage_id
								== operation.op1_storage_id) {
						instruction.source_op1_binding = {verified, -1};
					}
					break;
				}
				if (instruction.source_op1_binding.value_index < 0) {
					const int32_t reaching_phi = freeze_reaching_phi_value(
						plan, record.block_id, operation.op1_storage_id);
					if (reaching_phi >= 0) {
						instruction.source_op1_binding = {reaching_phi, -1};
					}
				}
			}
			const uint8_t source_opcode =
				source_position < plan->source_opcode_count
					? plan->source_opcodes[source_position].opcode
					: ZEND_NOP;
			instruction.local_abi_transport =
				record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
				&& (source_opcode == ZEND_RECV
					|| source_opcode == ZEND_RECV_INIT
					|| source_opcode == ZEND_RECV_VARIADIC);
		}
		if (instruction.direct_call != nullptr) {
			instruction.source_result_binding = {-1,
				static_cast<int32_t>(index)};
		} else if (instruction.direct_internal_call != nullptr
				&& instruction.call_site->source_do_opline_index
					< source_op_array->last) {
			const zend_mir_source_operand_ref &result_operand =
				instruction.direct_internal_call->result_operand;
			instruction.source_result_binding = freeze_source_value_binding(
				plan, source_op_array, result_operand,
				source_descriptor_storage(source_op_array, result_operand),
				instruction.call_site->source_do_opline_index, index,
				definitions);
		}
	}
	return true;
}

void freeze_dynamic_fetch_cv_indices(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa)
{
	if (source_op_array == nullptr || source_ssa == nullptr
			|| source_op_array->vars == nullptr
			|| source_op_array->opcodes == nullptr
			|| source_ssa->vars == nullptr || source_ssa->ops == nullptr) {
		return;
	}
	bool frame_aliases_stable = true;
	for (uint32_t source_index = 0;
			source_index < source_op_array->last; ++source_index) {
		switch (source_op_array->opcodes[source_index].opcode) {
			case ZEND_ASSIGN_REF:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_FCALL:
			case ZEND_SEND_REF:
			case ZEND_NEW:
			case ZEND_INIT_NS_FCALL_BY_NAME:
			case ZEND_INCLUDE_OR_EVAL:
			case ZEND_UNSET_VAR:
			case ZEND_FETCH_W:
			case ZEND_FETCH_RW:
			case ZEND_FETCH_FUNC_ARG:
			case ZEND_FETCH_UNSET:
			case ZEND_TICKS:
			case ZEND_INIT_METHOD_CALL:
			case ZEND_INIT_STATIC_METHOD_CALL:
			case ZEND_INIT_USER_CALL:
			case ZEND_FE_RESET_RW:
			case ZEND_FE_FETCH_RW:
			case ZEND_INIT_DYNAMIC_CALL:
			case ZEND_MAKE_REF:
			case ZEND_USER_OPCODE:
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
			case ZEND_BIND_GLOBAL:
			case ZEND_BIND_LEXICAL:
			case ZEND_BIND_STATIC:
				frame_aliases_stable = false;
				break;
			default:
				break;
		}
		if (!frame_aliases_stable) {
			break;
		}
	}
	auto source_position_dominates = [&](uint32_t definition,
			uint32_t use) -> bool {
		if (definition >= source_op_array->last || use >= source_op_array->last
				|| source_ssa->cfg.blocks == nullptr
				|| source_ssa->cfg.map == nullptr) {
			return false;
		}
		const uint32_t definition_block = source_ssa->cfg.map[definition];
		int32_t use_block = static_cast<int32_t>(source_ssa->cfg.map[use]);
		if (definition_block >= source_ssa->cfg.blocks_count
				|| use_block < 0
				|| static_cast<uint32_t>(use_block)
					>= source_ssa->cfg.blocks_count) {
			return false;
		}
		if (definition_block == static_cast<uint32_t>(use_block)) {
			return definition <= use;
		}
		while (use_block >= 0
				&& static_cast<uint32_t>(use_block) != definition_block) {
			use_block = source_ssa->cfg.blocks[use_block].idom;
		}
		return use_block >= 0
			&& static_cast<uint32_t>(use_block) == definition_block;
	};
	/* Per CV: whether a PHI defines it, and the SSA variables an opline
	 * defines for it in index order. Built on the first query. */
	std::vector<uint8_t> cv_has_phi;
	std::vector<std::vector<int32_t>> cv_definitions;
	std::unordered_map<std::string_view, uint32_t> cv_by_name;
	auto index_cvs = [&] {
		if (!cv_definitions.empty()) {
			return;
		}
		cv_has_phi.assign(source_op_array->last_var, 0);
		cv_definitions.resize(source_op_array->last_var);
		for (int32_t candidate = 0;
				candidate < source_ssa->vars_count; ++candidate) {
			const zend_ssa_var &variable = source_ssa->vars[candidate];
			if (variable.var < 0 || variable.var >= source_op_array->last_var) {
				continue;
			}
			const uint32_t cv = static_cast<uint32_t>(variable.var);
			if (variable.definition_phi != nullptr) {
				cv_has_phi[cv] = 1;
			}
			if (variable.definition >= 0) {
				cv_definitions[cv].push_back(candidate);
			}
		}
		for (uint32_t cv = source_op_array->last_var; cv-- > 0;) {
			cv_by_name.insert_or_assign(std::string_view{
				ZSTR_VAL(source_op_array->vars[cv]),
				ZSTR_LEN(source_op_array->vars[cv])}, cv);
		}
	};
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count; ++instruction_index) {
		zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				|| !instruction.has_value_operation) {
			continue;
		}
		const uint32_t ssa_variable =
			instruction.value_operation.op1.ssa_variable_id;
		if (!zend_mir_id_is_valid(ssa_variable)
				|| ssa_variable >= static_cast<uint32_t>(source_ssa->vars_count)
				|| !frame_aliases_stable) {
			continue;
		}
		const zend_ssa_var &name_variable = source_ssa->vars[ssa_variable];
		const int32_t definition = name_variable.definition;
		if (name_variable.definition_phi != nullptr || definition < 0
				|| static_cast<uint32_t>(definition) >= source_op_array->last) {
			continue;
		}
		const zend_op &op = source_op_array->opcodes[
			static_cast<uint32_t>(definition)];
		const zend_ssa_op &ssa_op = source_ssa->ops[
			static_cast<uint32_t>(definition)];
		if (op.opcode != ZEND_ASSIGN || op.op2_type != IS_CONST
				|| op.op1_type != IS_CV
				|| ssa_op.op1_def != static_cast<int32_t>(ssa_variable)
				|| !source_position_dominates(
					static_cast<uint32_t>(definition),
					record.source_position_id)) {
			continue;
		}
		const uint32_t name_cv = EX_VAR_TO_NUM(op.op1.var);
		index_cvs();
		if (name_cv >= source_op_array->last_var
				|| cv_has_phi[name_cv] != 0
				|| cv_definitions[name_cv].size() != 1) {
			continue;
		}
		const zval *literal = RT_CONSTANT(&op, op.op2);
		if (Z_TYPE_P(literal) != IS_STRING) {
			continue;
		}
		const auto named = cv_by_name.find(std::string_view{
			Z_STRVAL_P(literal), Z_STRLEN_P(literal)});
		if (named != cv_by_name.end()) {
			const uint32_t cv = named->second;
			{
				bool target_is_dominating_long = false;
				for (const int32_t candidate : cv_definitions[cv]) {
					const zend_ssa_var &target = source_ssa->vars[candidate];
					const uint32_t target_definition =
						static_cast<uint32_t>(target.definition);
					if (target_definition >= source_op_array->last) {
						continue;
					}
					const zend_op &target_op =
						source_op_array->opcodes[target_definition];
					const zend_ssa_op &target_ssa_op =
						source_ssa->ops[target_definition];
					if (target_op.opcode == ZEND_ASSIGN
							&& target_op.op2_type == IS_CONST
							&& target_ssa_op.op1_def == candidate
							&& Z_TYPE_P(RT_CONSTANT(
								&target_op, target_op.op2)) == IS_LONG
							&& source_position_dominates(
								target_definition,
								record.source_position_id)) {
						target_is_dominating_long = true;
					}
				}
				const bool target_is_stable = cv_has_phi[cv] == 0
					&& cv_definitions[cv].size() == 1;
				if (target_is_stable) {
					instruction.dynamic_fetch_cv_index = cv;
					instruction.dynamic_fetch_direct_long =
						target_is_dominating_long;
				}
			}
		}
	}
}

bool freeze_machine_references(
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag)
{

	std::vector<zend_tpde_machine_reference> references;
	/* Each distinct reference once, numbered in first-use order. */
	struct reference_hash {
		size_t operator()(const zend_tpde_machine_reference &reference) const {
			uint64_t hash = UINT64_C(1469598103934665603);
			for (const uint64_t field : {
					uint64_t{reference.kind}, uint64_t{reference.base_value_id},
					uint64_t{reference.index_value_id},
					uint64_t{reference.stable_storage_or_layout_id},
					uint64_t{reference.scale},
					static_cast<uint64_t>(reference.displacement),
					uint64_t{reference.access_width}}) {
				hash = (hash ^ field) * UINT64_C(1099511628211);
			}
			return static_cast<size_t>(hash);
		}
	};
	struct reference_equal {
		bool operator()(const zend_tpde_machine_reference &left,
				const zend_tpde_machine_reference &right) const {
			return left.kind == right.kind
				&& left.base_value_id == right.base_value_id
				&& left.index_value_id == right.index_value_id
				&& left.stable_storage_or_layout_id
					== right.stable_storage_or_layout_id
				&& left.scale == right.scale
				&& left.displacement == right.displacement
				&& left.access_width == right.access_width;
		}
	};
	std::unordered_map<zend_tpde_machine_reference, uint32_t,
		reference_hash, reference_equal> reference_indices;
	auto add_reference =
		[&](const zend_tpde_machine_reference &reference) -> uint32_t {
			if (const auto found = reference_indices.find(reference);
					found != reference_indices.end()) {
				return found->second;
			}
			if (references.size() >= MAX_RECORDS) {
				return UINT32_MAX;
			}
			references.push_back(reference);
			const uint32_t index =
				static_cast<uint32_t>(references.size() - 1);
			reference_indices.emplace(reference, index);
			return index;
		};
	auto binding_value_id =
		[&](const zend_tpde_source_value_binding &binding) {
			return binding.value_index >= 0
					&& static_cast<uint32_t>(binding.value_index)
						< plan->value_count
				? plan->values[
					static_cast<uint32_t>(binding.value_index)].id
				: ZEND_MIR_ID_INVALID;
		};
	auto operand_reference =
		[&](const zend_mir_source_operand_ref &operand,
				zend_mir_storage_id storage_id,
				const zend_tpde_source_value_binding &binding) {
			if (zend_mir_id_is_valid(storage_id)) {
				return add_reference({
					ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
					ZEND_MIR_ID_INVALID,
					ZEND_MIR_ID_INVALID,
					storage_id,
					1,
					static_cast<int64_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id)
							* sizeof(zval)),
					sizeof(zval),
				});
			}
			if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				return add_reference({
					ZEND_TPDE_MACHINE_REFERENCE_LITERAL,
					binding_value_id(binding),
					ZEND_MIR_ID_INVALID,
					operand.index,
					sizeof(zval),
					0,
					sizeof(zval),
				});
			}
			return UINT32_MAX;
		};

	/*
	 * Argument payload loads and safepoint materializations use canonical
	 * slots that no source opcode may name; the value table lists them so
	 * the adaptor never derives an address from the live op_array.
	 */
	for (uint32_t index = 0; index < plan->value_count; ++index) {
		const zend_tpde_value &value = plan->values[index];
		const zend_mir_storage_id storage_id = value.canonical_storage_id;
		if (!zend_mir_id_is_valid(storage_id)) {
			continue;
		}
		if (add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
				ZEND_MIR_ID_INVALID,
				ZEND_MIR_ID_INVALID,
				storage_id,
				1,
				static_cast<int64_t>(
					(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id)
						* sizeof(zval)),
				sizeof(zval),
			}) == UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-reference table exceeds the executable bound");
			return false;
		}
	}

	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		instruction.source_op1_reference_index = UINT32_MAX;
		instruction.source_op2_reference_index = UINT32_MAX;
		instruction.operation_reference_index = UINT32_MAX;
		if (!instruction.has_value_operation) {
			continue;
		}
		const zend_mir_executable_value_ref &operation =
			instruction.value_operation;
		instruction.source_op1_reference_index = operand_reference(
			operation.op1, operation.op1_storage_id,
			instruction.source_op1_binding);
		instruction.source_op2_reference_index = operand_reference(
			operation.op2, operation.op2_storage_id,
			instruction.source_op2_binding);
		/* The result and auxiliary slots stay in the reference table for
		 * lookups by storage. */
		operand_reference(
			operation.result, operation.result_storage_id,
			instruction.source_result_binding);
		operand_reference(
			operation.auxiliary, operation.auxiliary_storage_id,
			instruction.source_auxiliary_binding);

		if ((operation.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
					|| operation.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN)
				&& operation.op2.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			instruction.operation_reference_index = add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT,
				binding_value_id(instruction.source_op1_binding),
				binding_value_id(instruction.source_op2_binding),
				operation.extended_value & ~ZEND_FETCH_REF,
				1,
				0,
				sizeof(zval),
			});
		} else if (operation.opcode
					== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				|| operation.opcode
					== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM) {
			instruction.operation_reference_index = add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT,
				binding_value_id(instruction.source_op1_binding),
				operation.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					? binding_value_id(instruction.source_op2_binding)
					: ZEND_MIR_ID_INVALID,
				0,
				sizeof(zval),
				0,
				sizeof(zval),
			});
		}
		if ((instruction.operation_reference_index == UINT32_MAX
					&& (((operation.opcode
							== ZEND_MIR_OPCODE_OBJECT_FETCH_R
						|| operation.opcode
							== ZEND_MIR_OPCODE_OBJECT_ASSIGN)
						&& operation.op2.kind
							== ZEND_MIR_SOURCE_OPERAND_LITERAL)
						|| operation.opcode
							== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
						|| operation.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM))
				|| references.size() >= MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze a machine address reference");
			return false;
		}
	}

	plan->machine_reference_count =
		static_cast<uint32_t>(references.size());
	if (references.empty()) {
		return true;
	}
	plan->machine_references.reset(static_cast<zend_tpde_machine_reference *>(
		std::malloc(references.size()
			* sizeof(*plan->machine_references))));
	if (plan->machine_references == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the machine-reference table");
		return false;
	}
	std::memcpy(plan->machine_references, references.data(),
		references.size() * sizeof(*plan->machine_references));
	return true;
}

bool freeze_generator_resume_liveness(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_value_view *value_model,
	zend_native_diagnostic *diag)
{
	if (source_op_array == nullptr
			|| (source_op_array->fn_flags & ZEND_ACC_GENERATOR) == 0) {
		return true;
	}

	std::vector<uint32_t> targets;
	if ((source_op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) != 0) {
		for (uint32_t i = 0;
				i < source_op_array->last_try_catch; ++i) {
			const zend_try_catch_element &region =
				source_op_array->try_catch_array[i];
			if (region.finally_op != 0
					&& region.finally_op < source_op_array->last
					&& region.finally_end < source_op_array->last) {
				targets.push_back(region.finally_op);
			}
		}
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.has_value_operation
				&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
					|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					|| record.opcode
						== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			const uint32_t source_position =
				instruction.value_operation.source_position_id;
			if (source_position == UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"generator resume source position is invalid");
				return false;
			}
			targets.push_back(source_position + 1);
		}
	}
	std::sort(targets.begin(), targets.end());
	targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
	if (targets.empty()) {
		return true;
	}
	if (!checked_count(static_cast<uint32_t>(targets.size()))) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"generator resume target count exceeds the executable bound");
		return false;
	}

	plan->generator_resume_count = static_cast<uint32_t>(targets.size());
	plan->generator_resume_live_word_count =
		(plan->value_count + 63) / 64;
	const uint64_t live_word_count_u64 =
		static_cast<uint64_t>(plan->generator_resume_count)
			* plan->generator_resume_live_word_count;
	if (live_word_count_u64 > MAX_RECORDS) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"generator resume liveness exceeds the executable bound");
		return false;
	}
	plan->generator_resume_targets.reset(static_cast<uint32_t *>(
		std::malloc(targets.size()
			* sizeof(*plan->generator_resume_targets))));
	plan->generator_resume_landings.reset(static_cast<uint32_t *>(
		std::malloc(targets.size()
			* sizeof(*plan->generator_resume_landings))));
	plan->generator_resume_exception_blocks.reset(
		static_cast<zend_mir_block_id *>(std::malloc(
			targets.size()
				* sizeof(*plan->generator_resume_exception_blocks))));
	const size_t live_word_count =
		static_cast<size_t>(live_word_count_u64);
	plan->generator_resume_live_values.reset(
		live_word_count == 0 ? nullptr : static_cast<uint64_t *>(
			std::calloc(live_word_count,
				sizeof(*plan->generator_resume_live_values))));
	if (plan->generator_resume_targets == nullptr
			|| plan->generator_resume_landings == nullptr
			|| plan->generator_resume_exception_blocks == nullptr
			|| (live_word_count != 0
				&& plan->generator_resume_live_values == nullptr)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze generator resume liveness");
		return false;
	}

	/* A resume lands at the first MIR source position at or after its
	 * target; its suspensions must agree on their exception edge. */
	std::vector<uint32_t> source_positions;
	source_positions.reserve(plan->instruction_count);
	std::copy(targets.begin(), targets.end(),
		plan->generator_resume_targets.get());
	std::fill_n(plan->generator_resume_exception_blocks.get(),
		plan->generator_resume_count, ZEND_MIR_ID_INVALID);
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const uint32_t source_position = record.source_position_id;
		if (source_position == UINT32_MAX) {
			continue;
		}
		source_positions.push_back(source_position);
		if (!instruction.has_value_operation
				|| (record.opcode != ZEND_MIR_OPCODE_GENERATOR_CREATE
					&& record.opcode != ZEND_MIR_OPCODE_GENERATOR_YIELD
					&& record.opcode
						!= ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			continue;
		}
		const auto target = std::lower_bound(
			targets.begin(), targets.end(), source_position + 1);
		if (target == targets.end() || *target != source_position + 1) {
			continue;
		}
		zend_mir_block_id &exception_block =
			plan->generator_resume_exception_blocks[target - targets.begin()];
		if (zend_mir_id_is_valid(exception_block)
				&& exception_block != instruction.exception_block_id) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator resume has inconsistent exception edges");
			return false;
		}
		exception_block = instruction.exception_block_id;
	}
	std::sort(source_positions.begin(), source_positions.end());
	for (uint32_t resume = 0;
			resume < plan->generator_resume_count; ++resume) {
		const auto landing = std::lower_bound(source_positions.begin(),
			source_positions.end(), targets[resume]);
		if (landing == source_positions.end()) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator resume target has no MIR landing");
			return false;
		}
		plan->generator_resume_landings[resume] = *landing;
	}

	const uint32_t live_count =
		value_model->suspend_live_value_count(value_model->context);
	if (!checked_count(live_count)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"canonical suspend liveness exceeds the executable bound");
		return false;
	}
	for (uint32_t index = 0; index < live_count; ++index) {
		zend_mir_suspend_live_value_ref live{};
		if (!value_model->suspend_live_value_at(
				value_model->context, index, &live)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"canonical suspend liveness is unreadable");
			return false;
		}
		const auto target = std::lower_bound(
			targets.begin(), targets.end(),
			live.target_source_position_id);
		const int32_t value_index =
			zend_tpde_value_index(plan, live.value_id);
		if (target == targets.end()
				|| *target != live.target_source_position_id
				|| value_index < 0
				|| plan->values[value_index].canonical_storage_id
					!= live.storage_id) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"canonical suspend liveness does not match the machine plan");
			return false;
		}
		const uint32_t resume =
			static_cast<uint32_t>(target - targets.begin());
		/*
		 * The canonical table describes semantic liveness, including values
		 * whose authoritative copy lives in the generator frame; the adaptor
		 * takes it from here instead of rebuilding SSA liveness.
		 */
		uint64_t *words =
			plan->generator_resume_live_values
				+ static_cast<size_t>(resume)
					* plan->generator_resume_live_word_count;
		const uint32_t value = static_cast<uint32_t>(value_index);
		const zend_tpde_value &semantic_value = plan->values[value];
		if (!semantic_value.constant) {
			words[value / 64] |= uint64_t{1} << (value % 64);
		}
	}
	return true;
}

bool freeze_entry_undef_temporaries(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag)
{
	const zend_op_array *op_array = source_op_array;
	if (op_array == nullptr || op_array->T == 0) {
		return true;
	}
	if (op_array->last_live_range != 0 && op_array->live_range == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source live-range table is unreadable");
		return false;
	}

	std::vector<uint8_t> required(op_array->T);
	for (uint32_t index = 0; index < op_array->last_live_range; ++index) {
		const zend_live_range &range = op_array->live_range[index];
		const uint32_t kind = range.var & ZEND_LIVE_MASK;
		const uint32_t physical_slot =
			EX_VAR_TO_NUM(range.var & ~ZEND_LIVE_MASK);
		if (kind > ZEND_LIVE_NEW
				|| physical_slot < static_cast<uint32_t>(op_array->last_var)
				|| physical_slot
					>= static_cast<uint32_t>(op_array->last_var)
						+ op_array->T
				|| range.start >= range.end
				|| range.end > op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source live range is outside the executable frame");
			return false;
		}
		required[physical_slot
			- static_cast<uint32_t>(op_array->last_var)] = 1;
	}
	const uint32_t first_temporary =
		static_cast<uint32_t>(op_array->last_var);
	const uint64_t temporary_limit =
		static_cast<uint64_t>(first_temporary) + op_array->T;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		if (instruction.record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE) {
			continue;
		}
		const zend_mir_storage_id storage =
			instruction.zval_store_storage_id;
		if (!zend_mir_id_is_valid(storage)
				|| static_cast<uint64_t>(storage) >= temporary_limit) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"zval store destination is outside the executable frame");
			return false;
		}
		if (storage < first_temporary) {
			continue;
		}
		required[storage - first_temporary] = 1;
	}
	/*
	 * Explicit value helpers require a fresh destination before they
	 * allocate, call user code or consume an operand, so their result
	 * temporaries are initialized at entry, not every temporary.
	 * fused_need marks those only branch-fused helpers need (see below).
	 */
	std::vector<uint8_t> fused_need(op_array->T);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		if (!instruction.has_value_operation
				|| instruction.runtime_helper == ZEND_NATIVE_HELPER_COUNT
				|| !zend_tpde_helper_has_explicit_operands(
					instruction.runtime_helper)) {
			continue;
		}
		const zend_mir_storage_id storage =
			instruction.value_operation.result_storage_id;
		if (!zend_mir_id_is_valid(storage)) {
			continue;
		}
		if (static_cast<uint64_t>(storage) >= temporary_limit) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"value helper result is outside the temporary frame");
			return false;
		}
		if (storage >= first_temporary) {
			/* A helper of an operation fused into its branch runs after
			 * the branch stored a placeholder into its result (Zend
			 * entry), which is as fresh as an initialized one. */
			if (instruction.fused_into_branch) {
				if (required[storage - first_temporary] == 0) {
					fused_need[storage - first_temporary] = 1;
				}
				continue;
			}
			required[storage - first_temporary] = 1;
		}
	}
	if (plan->user_opcode_callbacks
			&& plan->user_opcode_source_operations != nullptr) {
		for (uint32_t source = 0;
				source < plan->user_opcode_source_operation_count; ++source) {
			if (zend_get_user_opcode_handler(
					op_array->opcodes[source].opcode) == nullptr) {
				continue;
			}
			const zend_mir_source_operand_ref &result =
				plan->user_opcode_source_operations[source].result;
			if ((result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
						|| result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
					&& (result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
						|| result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
					&& result.index < op_array->T) {
				required[result.index] = 1;
			}
		}
	}

	std::vector<uint32_t> indices;
	std::vector<uint8_t> fused_only;
	indices.reserve(op_array->T);
	for (uint32_t index = 0; index < op_array->T; ++index) {
		if (required[index] != 0 || fused_need[index] != 0) {
			indices.push_back(index);
			fused_only.push_back(required[index] == 0);
		}
	}
	plan->entry_undef_temporary_count =
		static_cast<uint32_t>(indices.size());
	if (indices.empty()) {
		return true;
	}
	plan->entry_undef_temporary_indices.reset(static_cast<uint32_t *>(
		std::malloc(indices.size()
			* sizeof(*plan->entry_undef_temporary_indices))));
	plan->entry_undef_temporary_fused_only.reset(static_cast<uint8_t *>(
		std::malloc(fused_only.size())));
	if (plan->entry_undef_temporary_indices == nullptr
			|| plan->entry_undef_temporary_fused_only == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate selective temporary initialization plan");
		return false;
	}
	std::memcpy(plan->entry_undef_temporary_indices, indices.data(),
		indices.size() * sizeof(*plan->entry_undef_temporary_indices));
	std::memcpy(plan->entry_undef_temporary_fused_only, fused_only.data(),
		fused_only.size());
	return true;
}

} // namespace zend::native::tpde::plan_detail
