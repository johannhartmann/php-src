// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

/*
 * A resume target that begins a block names that block's PHIs and the COPYs
 * at its head. The suspension in a predecessor block publishes the value they
 * receive on its edge: the definitions themselves run only after the resume.
 */
uint32_t generator_suspend_value_index(
	const zend_tpde_plan *plan, const int32_t *definitions,
	uint32_t suspend_instruction, uint32_t value_index)
{
	if (definitions == nullptr
			|| plan->block_predecessor_offsets == nullptr
			|| plan->block_predecessors == nullptr) {
		return value_index;
	}
	const zend_mir_instruction_record suspend_record =
		zend_tpde_instruction_record_at(
			plan, &plan->instructions[suspend_instruction]);
	const int32_t suspend_block =
		zend_tpde_block_index(plan, suspend_record.block_id);
	for (uint32_t depth = 0; depth <= plan->value_count; ++depth) {
		const int32_t definition = definitions[value_index];
		if (suspend_block < 0 || definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count) {
			return value_index;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const int32_t block = zend_tpde_block_index(plan, record.block_id);
		if (block < 0 || block == suspend_block
				|| (record.opcode != ZEND_MIR_OPCODE_PHI
					&& (record.opcode != ZEND_MIR_OPCODE_COPY
						|| instruction.operand_count != 1))) {
			return value_index;
		}
		const uint32_t begin = plan->block_predecessor_offsets[block];
		const uint32_t end = plan->block_predecessor_offsets[block + 1];
		uint32_t edge = end;
		for (uint32_t predecessor = begin; predecessor < end; ++predecessor) {
			if (plan->block_predecessors[predecessor]
					== static_cast<uint32_t>(suspend_block)) {
				edge = predecessor;
				break;
			}
		}
		const uint32_t operand = record.opcode == ZEND_MIR_OPCODE_PHI
			? edge - begin : 0;
		if (edge == end || operand >= instruction.operand_count) {
			return value_index;
		}
		const int32_t input = zend_tpde_value_index(plan,
			zend_tpde_operand_at(plan, &instruction, operand));
		if (input < 0) {
			return value_index;
		}
		value_index = static_cast<uint32_t>(input);
	}
	return value_index;
}

bool freeze_machine_plan_consumers(
	zend_tpde_plan *plan, zend_native_diagnostic *diag)
{
	const uint32_t value_count = plan->value_count;
	std::vector<uint32_t> counts(value_count, 0);

	plan->value_definition_instructions.reset(value_count == 0
		? nullptr : static_cast<int32_t *>(std::malloc(
			static_cast<size_t>(value_count)
				* sizeof(*plan->value_definition_instructions))));
	plan->source_value_definition_instructions.reset(value_count == 0
		? nullptr : static_cast<int32_t *>(std::malloc(
			static_cast<size_t>(value_count)
				* sizeof(*plan->source_value_definition_instructions))));
	plan->value_consumer_offsets.reset(static_cast<uint32_t *>(std::calloc(
		static_cast<size_t>(value_count) + 1,
		sizeof(*plan->value_consumer_offsets))));
	if ((value_count != 0
				&& (plan->value_definition_instructions == nullptr
					|| plan->source_value_definition_instructions == nullptr))
			|| plan->value_consumer_offsets == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze machine-plan consumers");
		return false;
	}
	if (value_count != 0) {
		std::fill_n(plan->value_definition_instructions.get(),
			value_count, int32_t{-1});
		std::fill_n(plan->source_value_definition_instructions.get(),
			value_count, int32_t{-1});
	}

	auto visit_use = [&](zend_mir_value_id id,
			uint32_t instruction_index, uint32_t operand_index,
			uint32_t auxiliary, zend_tpde_machine_use_kind kind, bool fill,
			std::vector<uint32_t> *cursors) -> bool {
		if (!zend_mir_id_is_valid(id)) {
			return true;
		}
		const int32_t value_index = zend_tpde_value_index(plan, id);
		if (value_index < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan consumer references an unknown value");
			return false;
		}
		const uint32_t value = static_cast<uint32_t>(value_index);
		if (!fill) {
			if (counts[value] == UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan consumer count overflows");
				return false;
			}
			++counts[value];
		} else {
			plan->value_consumers[(*cursors)[value]++] = {
				instruction_index,
				operand_index,
				auxiliary,
				kind,
			};
		}
		return true;
	};
	auto visit_instruction_uses = [&](uint32_t instruction_index,
			bool fill, std::vector<uint32_t> *cursors) -> bool {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			if (!visit_use(zend_tpde_operand_at(
					plan, &instruction, n), instruction_index, n,
					record.opcode == ZEND_MIR_OPCODE_PHI ? n : UINT32_MAX,
					record.opcode == ZEND_MIR_OPCODE_PHI
						? ZEND_TPDE_MACHINE_USE_PHI_EDGE
						: ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND,
					fill, cursors)) {
				return false;
			}
		}
		if (instruction.has_value_operation) {
			const zend_tpde_source_value_binding bindings[] = {
				instruction.source_op1_binding,
				instruction.source_op2_binding,
				instruction.source_auxiliary_binding,
			};
			for (uint32_t n = 0; n < 3; ++n) {
				const int32_t value_index = bindings[n].value_index;
				if (value_index < 0) {
					continue;
				}
				if (static_cast<uint32_t>(value_index) >= plan->value_count
						|| !visit_use(
							plan->values[static_cast<uint32_t>(value_index)].id,
							instruction_index, n, UINT32_MAX,
							ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND,
							fill, cursors)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source operand binding references an unknown value");
					return false;
				}
			}
		}
		for (uint32_t n = 0;
				n < instruction.call_argument_count; ++n) {
			zend_mir_call_argument_ref argument{};
			if (!zend_tpde_call_argument_at(plan,
					instruction.call_argument_offset + n, &argument)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan call argument table is unreadable");
				return false;
			}
			if (!visit_use(argument.value_id, instruction_index, n,
					instruction.component_target_index != UINT32_MAX
							&& record.opcode
								== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						? instruction.component_target_index
						: UINT32_MAX,
					instruction.component_target_index != UINT32_MAX
							&& record.opcode
								== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						? ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT
						: ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT,
					fill, cursors)) {
				return false;
			}
		}
		if (instruction.materialization_offset
					> plan->materialization_count
				|| instruction.materialization_count
					> plan->materialization_count
						- instruction.materialization_offset) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan materialization slice is invalid");
			return false;
		}
		for (uint32_t n = 0;
				n < instruction.materialization_count; ++n) {
			const zend_tpde_materialization &materialization =
				plan->materializations[
					instruction.materialization_offset + n];
			if (materialization.value_index == UINT32_MAX) {
				if (materialization
								.source_definition_instruction_index < 0
						|| static_cast<uint32_t>(materialization
							.source_definition_instruction_index)
							>= plan->instruction_count
						|| (materialization.source_value_index >= 0
							&& static_cast<uint32_t>(
								materialization.source_value_index)
								>= plan->value_count)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"machine-plan source materialization has no definition");
					return false;
				}
				continue;
			}
			if (materialization.value_index >= plan->value_count
					|| !visit_use(
						plan->values[materialization.value_index].id,
						instruction_index, n,
						materialization.storage_id,
						ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION,
						fill, cursors)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan materialization references an unknown value");
				return false;
			}
		}
		if (instruction.has_value_operation
				&& record.source_position_id != UINT32_MAX
				&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
					|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					|| record.opcode
						== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			const uint32_t resume = generator_resume_at_target(
				plan, record.source_position_id + 1);
			if (!zend_tpde_visit_generator_resume_values(plan, resume,
					[&](uint32_t value) {
						return visit_use(plan->values[
								generator_suspend_value_index(plan,
									plan->value_definition_instructions,
									instruction_index, value)].id,
							instruction_index, value, resume,
							ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE,
							fill, cursors);
					})) {
				return false;
			}
		}
		return true;
	};

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(
				plan, &plan->instructions[instruction_index]);
		if (zend_mir_id_is_valid(record.result_id)) {
			const int32_t value_index =
				zend_tpde_value_index(plan, record.result_id);
			if (value_index < 0
					|| plan->value_definition_instructions[
						static_cast<uint32_t>(value_index)] >= 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan value has an invalid or duplicate definition");
				return false;
			}
			plan->value_definition_instructions[
				static_cast<uint32_t>(value_index)] =
				static_cast<int32_t>(instruction_index);
		}
		const int32_t source_result =
			plan->instructions[instruction_index]
				.source_result_binding.value_index;
		if (source_result >= 0
				&& static_cast<uint32_t>(source_result) < value_count
				&& plan->source_value_definition_instructions[
					static_cast<uint32_t>(source_result)] < 0) {
			plan->source_value_definition_instructions[
				static_cast<uint32_t>(source_result)] =
				static_cast<int32_t>(instruction_index);
		}
		if (!visit_instruction_uses(instruction_index, false, nullptr)) {
			return false;
		}
	}

	uint64_t consumer_count = 0;
	for (uint32_t value = 0; value < value_count; ++value) {
		plan->value_consumer_offsets[value] =
			static_cast<uint32_t>(consumer_count);
		consumer_count += counts[value];
		if (consumer_count > MAX_RECORDS || consumer_count > UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan consumer table exceeds the executable bound");
			return false;
		}
	}
	plan->value_consumer_offsets[value_count] =
		static_cast<uint32_t>(consumer_count);
	plan->value_consumers.reset(consumer_count == 0
		? nullptr : static_cast<zend_tpde_machine_use *>(std::malloc(
			static_cast<size_t>(consumer_count)
				* sizeof(*plan->value_consumers))));
	if (consumer_count != 0 && plan->value_consumers == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate machine-plan consumer table");
		return false;
	}
	std::vector<uint32_t> cursors(
		plan->value_consumer_offsets.get(),
		plan->value_consumer_offsets.get() + value_count);
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		if (!visit_instruction_uses(
				instruction_index, true, &cursors)) {
			return false;
		}
	}
	return true;
}

static int32_t machine_source_binding_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_source_value_binding &binding) {
	if (binding.value_index >= 0
			&& static_cast<uint32_t>(binding.value_index)
				< plan->value_count) {
		return binding.value_index;
	}
	if (binding.definition_instruction_index < 0
			|| static_cast<uint32_t>(binding.definition_instruction_index)
				>= plan->instruction_count) {
		return -1;
	}
	const zend_tpde_source_value_binding &result =
		plan->instructions[static_cast<uint32_t>(
			binding.definition_instruction_index)].source_result_binding;
	return result.value_index >= 0
			&& static_cast<uint32_t>(result.value_index) < plan->value_count
		? result.value_index : -1;
}

static bool machine_short_circuit_boolean_phi_consumer(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use) {
	if (use.kind != ZEND_TPDE_MACHINE_USE_PHI_EDGE
			|| use.instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &phi =
		plan->instructions[use.instruction_index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &phi);
	if (record.opcode != ZEND_MIR_OPCODE_PHI
			|| phi.operand_count != 2
			|| use.operand_index >= phi.operand_count) {
		return false;
	}
	const uint32_t edge_operand = use.operand_index == 0 ? 1 : 0;
	const int32_t edge_value = zend_tpde_value_index(
		plan, zend_tpde_operand_at(plan, &phi, edge_operand));
	if (edge_value < 0) {
		return false;
	}
	int32_t producer = plan->value_definition_instructions == nullptr
		? -1 : plan->value_definition_instructions[edge_value];
	if (producer < 0 && plan->source_value_definition_instructions != nullptr) {
		/* The first instruction whose source result is the value. */
		producer = plan->source_value_definition_instructions[edge_value];
	}
	if (producer < 0
			|| static_cast<uint32_t>(producer) >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &branch =
		plan->instructions[static_cast<uint32_t>(producer)];
	const zend_mir_instruction_record branch_record =
		zend_tpde_instruction_record_at(plan, &branch);
	return branch.has_value_operation
		&& branch_record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
		&& (branch.value_operation.source_opcode == ZEND_JMPZ_EX
			|| branch.value_operation.source_opcode == ZEND_JMPNZ_EX)
		&& (branch.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
}

static bool machine_register_boolean_consumer(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use,
		int32_t producer_value_index,
		uint32_t producer_instruction_index) {
	if (machine_short_circuit_boolean_phi_consumer(plan, use)) {
		return true;
	}
	if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
			|| use.instruction_index >= plan->instruction_count
			|| use.operand_index != 0) {
		return false;
	}
	const zend_tpde_instruction &consumer =
		plan->instructions[use.instruction_index];
	if (!consumer.has_value_operation
			|| machine_source_binding_value_index(
				plan, consumer.source_op1_binding)
				!= producer_value_index) {
		return false;
	}
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &consumer);
	if (producer_instruction_index >= plan->instruction_count
			|| zend_tpde_instruction_record_at(plan,
				&plan->instructions[producer_instruction_index]).block_id
				!= record.block_id) {
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
		/*
		 * A terminal branch becomes a register consumer once its producer is
		 * selected. A short-circuit branch with a result must have passed the
		 * register-only edge check, or it would read an unmaterialized zval.
		 */
		if (consumer.value_operation.source_opcode == ZEND_JMPZ
				|| consumer.value_operation.source_opcode == ZEND_JMPNZ) {
			return true;
		}
		return (consumer.value_operation.source_opcode == ZEND_JMPZ_EX
				|| consumer.value_operation.source_opcode == ZEND_JMPNZ_EX)
			&& (consumer.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
		return consumer.value_operation.source_opcode == ZEND_BOOL
			|| consumer.value_operation.source_opcode == ZEND_BOOL_NOT;
	}
	return false;
}

static void freeze_register_boolean_terminal_consumers(
		zend_tpde_plan *plan, int32_t producer_value_index) {
	if (producer_value_index < 0
			|| static_cast<uint32_t>(producer_value_index) >= plan->value_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return;
	}
	const uint32_t value = static_cast<uint32_t>(producer_value_index);
	const uint32_t begin = plan->value_consumer_offsets[value];
	const uint32_t end = plan->value_consumer_offsets[value + 1];
	for (uint32_t use_index = begin; use_index < end; ++use_index) {
		const zend_tpde_machine_use &use = plan->value_consumers[use_index];
		if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
				|| use.operand_index != 0
				|| use.instruction_index >= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &consumer =
			plan->instructions[use.instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &consumer);
		if (consumer.has_value_operation
				&& record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& machine_source_binding_value_index(
					plan, consumer.source_op1_binding) == producer_value_index
				&& (consumer.value_operation.source_opcode == ZEND_JMPZ
					|| consumer.value_operation.source_opcode == ZEND_JMPNZ)) {
			consumer.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
		}
	}
}

static int32_t machine_single_source_consumer(
		const zend_tpde_plan *plan, int32_t value_index) {
	if (value_index < 0
			|| static_cast<uint32_t>(value_index) >= plan->value_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return -1;
	}
	const uint32_t begin =
		plan->value_consumer_offsets[static_cast<uint32_t>(value_index)];
	const uint32_t end =
		plan->value_consumer_offsets[static_cast<uint32_t>(value_index) + 1];
	int32_t consumer = -1;
	bool source_operand = false;
	for (uint32_t index = begin; index < end; ++index) {
		const zend_tpde_machine_use &use = plan->value_consumers[index];
		if ((use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
				&& use.kind != ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND)
				|| (use.kind == ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
					&& use.operand_index != 0)
				|| (consumer >= 0
					&& static_cast<uint32_t>(consumer)
						!= use.instruction_index)) {
			return -1;
		}
		consumer = static_cast<int32_t>(use.instruction_index);
		source_operand = source_operand
			|| use.kind == ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND;
	}
	return source_operand ? consumer : -1;
}

static bool machine_short_circuit_branch_is_register_only(
		const zend_tpde_plan *plan, uint32_t branch_index) {
	if (branch_index >= plan->instruction_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return false;
	}
	const zend_tpde_instruction &branch = plan->instructions[branch_index];
	const int32_t result_index = machine_source_binding_value_index(
		plan, branch.source_result_binding);
	if (result_index < 0) {
		return false;
	}
	const uint32_t value = static_cast<uint32_t>(result_index);
	const uint32_t begin = plan->value_consumer_offsets[value];
	const uint32_t end = plan->value_consumer_offsets[value + 1];
	if (begin == end) {
		return false;
	}
	for (uint32_t use_index = begin; use_index < end; ++use_index) {
		const zend_tpde_machine_use &use =
			plan->value_consumers[use_index];
		if (use.kind != ZEND_TPDE_MACHINE_USE_PHI_EDGE
				|| use.instruction_index >= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &phi =
			plan->instructions[use.instruction_index];
		const zend_mir_instruction_record phi_record =
			zend_tpde_instruction_record_at(plan, &phi);
		if (phi_record.opcode != ZEND_MIR_OPCODE_PHI
				|| phi.operand_count != 2) {
			return false;
		}
		const int32_t phi_result =
			zend_tpde_value_index(plan, phi_record.result_id);
		const int32_t consumer_index =
			machine_single_source_consumer(plan, phi_result);
		if (consumer_index < 0
				|| static_cast<uint32_t>(consumer_index)
					>= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &consumer =
			plan->instructions[static_cast<uint32_t>(consumer_index)];
		const zend_mir_instruction_record consumer_record =
			zend_tpde_instruction_record_at(plan, &consumer);
		if (!consumer.has_value_operation
				|| consumer_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (consumer.value_operation.source_opcode != ZEND_JMPZ
					&& consumer.value_operation.source_opcode != ZEND_JMPNZ)) {
			return false;
		}
	}
	return true;
}

/*
 * A guarded numeric comparison or CV TYPE_CHECK feeding only the
 * JMPZ/JMPNZ that follows it would box a boolean in its hot path for the
 * branch to test again. Let the branch evaluate it instead, like the VM's
 * smart branches: operands that are both numbers compare and a type tests
 * its mask inline; the helper computes anything else into the temporary
 * the branch then tests.
 */
void freeze_fused_compare_branches(zend_tpde_plan *plan)
{
	if (plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return;
	}
	for (uint32_t index = 0; index + 1 < plan->instruction_count; ++index) {
		zend_tpde_instruction &compare = plan->instructions[index];
		zend_tpde_instruction &branch = plan->instructions[index + 1];
		const zend_mir_executable_value_ref &operation =
			compare.value_operation;
		/* The branch reads a fused TYPE_CHECK's CV from its slot: the check
		 * must have no machine operand, as its inline form requires. */
		zend_tpde_fused_operand checked{};
		zend_tpde_fused_operand other{};
		const bool type_check =
			zend_tpde_fused_type_check_at(compare, &checked)
			&& compare.operand_count == 0;
		/* isset($cv), isset($a[$k]) and ===/!== are tested by the branch
		 * the same way. */
		const bool isset_cv = zend_tpde_fused_isset_cv_at(compare, &checked);
		const bool isset_dim =
			zend_tpde_fused_isset_dim_at(compare, &checked, &other);
		const bool identical =
			zend_tpde_fused_identical_at(compare, &checked, &other);
		if (!compare.has_value_operation || !branch.has_value_operation
				|| (!type_check && !isset_cv && !isset_dim && !identical
					&& (operation.opcode != ZEND_MIR_OPCODE_VALUE_BINARY_OP
						|| (operation.source_opcode != ZEND_IS_SMALLER
							&& operation.source_opcode
								!= ZEND_IS_SMALLER_OR_EQUAL
							&& operation.source_opcode != ZEND_IS_EQUAL
							&& operation.source_opcode
								!= ZEND_IS_NOT_EQUAL)))
				|| (compare.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) == 0
				|| (compare.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
				|| operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| branch.value_operation.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)
				|| (branch.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) == 0
				|| branch.value_operation.op1_storage_id
					!= operation.result_storage_id
				|| compare.record.block_id != branch.record.block_id) {
			continue;
		}
		zend_tpde_fused_operand left{};
		zend_tpde_fused_operand right{};
		zend_tpde_value_condition condition{};
		/* A branch with its own guard and cold blocks keeps its form. */
		if ((!type_check && !isset_cv && !isset_dim
					&& !zend_tpde_fused_compare_at(compare, &left, &right))
				|| zend_tpde_value_condition_at(branch, &condition)) {
			continue;
		}
		const int32_t result_index = machine_source_binding_value_index(
			plan, compare.source_result_binding);
		if (result_index < 0
				|| machine_single_source_consumer(plan, result_index)
					!= static_cast<int32_t>(index + 1)) {
			continue;
		}
		compare.fused_into_branch = true;
		branch.fused_compare_plus_one = index + 1;
	}
}

/* A frame slot operand: a CV or temporary with its storage. */
static bool fused_slot_operand(const zend_mir_source_operand_ref &operand,
		zend_mir_storage_id storage, zend_mir_source_slot_kind kind) {
	return (operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
			|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
		&& operand.slot_kind == kind && zend_mir_id_is_valid(storage);
}

/* A class constant of a literal name, of a literal class or of self::,
 * parent:: or static::, as class_constant_inline() reads it. */
static bool fused_class_constant(const zend_tpde_instruction &producer) {
	const zend_mir_executable_value_ref &operation = producer.value_operation;
	const uint32_t fetch =
		operation.op1_unused_payload & ZEND_FETCH_CLASS_MASK;
	return operation.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_CONSTANT
		&& operation.source_opcode == ZEND_FETCH_CLASS_CONSTANT
		&& operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
		&& (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (fetch == ZEND_FETCH_CLASS_SELF
					|| fetch == ZEND_FETCH_CLASS_PARENT
					|| fetch == ZEND_FETCH_CLASS_STATIC)))
		&& operation.extended_value <= INT32_MAX - 2 * sizeof(void *);
}

/*
 * A producer whose value an array read can use in place: a cached
 * declared property read, a class constant, or an element read of a CV,
 * a literal or a fused producer's value under a literal or CV key.
 */
static bool fused_container_producer(const zend_tpde_instruction &producer) {
	const zend_mir_executable_value_ref &operation = producer.value_operation;
	zend_tpde_object_property_read property{};
	if (zend_tpde_object_property_read_at(producer, &property)
			|| zend_tpde_object_property_isset_read_at(producer, &property)
			|| fused_class_constant(producer)) {
		return true;
	}
	return ((operation.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				&& operation.source_opcode == ZEND_FETCH_DIM_R)
			|| (operation.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS
				&& operation.source_opcode == ZEND_FETCH_DIM_IS))
		&& (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| fused_slot_operand(operation.op1, operation.op1_storage_id,
				ZEND_MIR_SOURCE_SLOT_CV)
			|| (producer.fused_container_plus_one != 0
				&& fused_slot_operand(operation.op1,
					operation.op1_storage_id, ZEND_MIR_SOURCE_SLOT_TMP)))
		&& (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| fused_slot_operand(operation.op2, operation.op2_storage_id,
				ZEND_MIR_SOURCE_SLOT_CV))
		&& operation.op1_storage_id != operation.result_storage_id
		&& operation.op2_storage_id != operation.result_storage_id;
}

/*
 * A property, class constant or element read whose temporary only feeds
 * the FETCH_DIM_R, FETCH_DIM_IS, isset() or empty() right after it would
 * copy the value there, counting it, for the consumer to probe it and
 * release it again.
 * Let the consumer evaluate the producer and probe the value in place; a
 * temporary exists only on the way to the helpers (Linux x64).
 */
void freeze_fused_container_reads(zend_tpde_plan *plan)
{
	if (!zend_tpde_linux_inline_forms || plan->user_opcode_callbacks
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return;
	}
	for (uint32_t index = 0; index + 1 < plan->instruction_count; ++index) {
		zend_tpde_instruction &producer = plan->instructions[index];
		zend_tpde_instruction &consumer = plan->instructions[index + 1];
		const zend_mir_executable_value_ref &produced =
			producer.value_operation;
		const zend_mir_executable_value_ref &consumed =
			consumer.value_operation;
		/* A class constant fetch keeps its helper in line. */
		if (!producer.has_value_operation || !consumer.has_value_operation
				|| ((producer.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) == 0
					&& !fused_class_constant(producer))
				|| (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
				|| (consumer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) == 0
				|| consumer.fused_into_branch
				|| !((consumed.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
						&& consumed.source_opcode == ZEND_FETCH_DIM_R)
					|| (consumed.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS
						&& consumed.source_opcode == ZEND_FETCH_DIM_IS)
					|| (consumed.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
						&& consumed.source_opcode
							== ZEND_ISSET_ISEMPTY_DIM_OBJ))
				|| produced.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				|| !zend_mir_id_is_valid(produced.result_storage_id)
				|| !fused_slot_operand(consumed.op1, consumed.op1_storage_id,
					ZEND_MIR_SOURCE_SLOT_TMP)
				|| consumed.op1_storage_id != produced.result_storage_id
				|| consumed.op2_storage_id == produced.result_storage_id
				/* The forms array_element() lowers: a literal or slot key
				 * and a temporary result in its own slot. */
				|| (consumed.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
					&& !fused_slot_operand(consumed.op2,
						consumed.op2_storage_id, ZEND_MIR_SOURCE_SLOT_CV)
					&& !fused_slot_operand(consumed.op2,
						consumed.op2_storage_id, ZEND_MIR_SOURCE_SLOT_TMP))
				|| (consumed.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& consumed.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
				|| !zend_mir_id_is_valid(consumed.result_storage_id)
				|| consumed.result_storage_id == consumed.op1_storage_id
				|| consumed.result_storage_id == consumed.op2_storage_id
				|| producer.record.block_id != consumer.record.block_id
				|| !fused_container_producer(producer)) {
			continue;
		}
		/* The temporary has no register value that a statepoint would
		 * store over the placeholders the consumer leaves there. */
		const int32_t result_index = machine_source_binding_value_index(
			plan, producer.source_result_binding);
		if (result_index < 0
				|| plan->values[static_cast<uint32_t>(result_index)]
					.register_authoritative
				|| machine_single_source_consumer(plan, result_index)
					!= static_cast<int32_t>(index + 1)) {
			continue;
		}
		producer.fused_into_container = true;
		consumer.fused_container_plus_one = index + 1;
		require_runtime_helper(plan, ZEND_NATIVE_HELPER_VALUE_FUSED_CONTAINER);
		/* The consumer's statepoint published the producer's value into
		 * the temporary; the markers and their helper own it now. */
		zend_tpde_materialization *records = plan->materializations.get()
			+ consumer.materialization_offset;
		uint32_t kept = 0;
		for (uint32_t record = 0; record < consumer.materialization_count;
				++record) {
			if (records[record].storage_id != produced.result_storage_id) {
				records[kept++] = records[record];
			}
		}
		consumer.materialization_count = kept;
	}
}

/* The MIR definition, else the first instruction whose source result
 * is the value. */
static int32_t register_boolean_value_definition(
		const zend_tpde_plan *plan, int32_t value_index) {
	if (value_index < 0
			|| static_cast<uint32_t>(value_index) >= plan->value_count) {
		return int32_t{-1};
	}
	const int32_t definition = plan->value_definition_instructions != nullptr
		? plan->value_definition_instructions[value_index] : -1;
	return definition >= 0 || plan->source_value_definition_instructions
			== nullptr
		? definition
		: plan->source_value_definition_instructions[value_index];
}

/* A JMPZ_EX or JMPNZ_EX whose result reaches only register-only PHIs
 * branches on a register. */
static void freeze_register_short_circuit_branches(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.has_value_operation
				&& record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& (instruction.value_operation.source_opcode == ZEND_JMPZ_EX
					|| instruction.value_operation.source_opcode == ZEND_JMPNZ_EX)
				&& machine_short_circuit_branch_is_register_only(plan, index)) {
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
		}
	}
}

/* The instruction's boolean result becomes a register result when every
 * consumer of it reads a register boolean. */
static void freeze_register_boolean_result(
		zend_tpde_plan *plan,
		std::vector<uint8_t> &register_boolean_results,
		uint32_t index, int32_t result_index) {
	zend_tpde_instruction &instruction = plan->instructions[index];
	const uint32_t value = static_cast<uint32_t>(result_index);
	const uint32_t begin = plan->value_consumer_offsets[value];
	const uint32_t end = plan->value_consumer_offsets[value + 1];
	if (begin == end) {
		return;
	}
	bool all_register_consumers = true;
	for (uint32_t use = begin; use < end; ++use) {
		if (!machine_register_boolean_consumer(
				plan, plan->value_consumers[use], result_index, index)) {
			all_register_consumers = false;
			break;
		}
	}
	if (all_register_consumers) {
		instruction.machine_control_flow_flags &=
			~ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
		instruction.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT;
		register_boolean_results[index] = 1;
		freeze_register_boolean_terminal_consumers(plan, result_index);
	}
}

/* isset/empty of a CV that holds null, a boolean or a long. */
static void freeze_register_isset_empty_results(
		zend_tpde_plan *plan,
		std::vector<uint8_t> &register_boolean_results) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!instruction.has_value_operation
				|| record.opcode
					!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				|| instruction.value_operation.source_opcode
					!= ZEND_ISSET_ISEMPTY_CV
				|| instruction.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
			continue;
		}
		const int32_t input_index = machine_source_binding_value_index(
			plan, instruction.source_op1_binding);
		const int32_t result_index = machine_source_binding_value_index(
			plan, instruction.source_result_binding);
		if (input_index < 0 || result_index < 0) {
			continue;
		}
		const zend_tpde_value &input =
			plan->values[static_cast<uint32_t>(input_index)];
		if ((input.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& input.exact_type != ZEND_MIR_SCALAR_TYPE_I1
				&& input.exact_type != ZEND_MIR_SCALAR_TYPE_I64)
				|| input.canonical_alias_observable
				|| input.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
			continue;
		}
		zend_tpde_slot_isset_empty isset_layout{};
		if (!zend_tpde_slot_isset_empty_at(instruction, &isset_layout)) {
			continue;
		}
		freeze_register_boolean_result(
			plan, register_boolean_results, index, result_index);
	}
}

/*
 * BOOL and BOOL_NOT of a machine boolean are exact. Propagate the
 * register result through such chains so that no negation between
 * isset/empty and its branch gets a guarded helper diamond.
 */
static void freeze_register_bool_chains(
		zend_tpde_plan *plan,
		std::vector<uint8_t> &register_boolean_results) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!instruction.has_value_operation
				|| record.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
				|| (instruction.value_operation.source_opcode != ZEND_BOOL
					&& instruction.value_operation.source_opcode
						!= ZEND_BOOL_NOT)) {
			continue;
		}
		int32_t producer =
			instruction.source_op1_binding.definition_instruction_index;
		if (producer < 0) {
			producer = register_boolean_value_definition(plan,
				machine_source_binding_value_index(
					plan, instruction.source_op1_binding));
		}
		if (producer < 0
				|| static_cast<uint32_t>(producer) >= index
				|| register_boolean_results[
					static_cast<uint32_t>(producer)] == 0) {
			continue;
		}
		const int32_t result_index = machine_source_binding_value_index(
			plan, instruction.source_result_binding);
		if (result_index < 0) {
			continue;
		}
		freeze_register_boolean_result(
			plan, register_boolean_results, index, result_index);
	}
}

/*
 * Fold empty(CV) -> BOOL_NOT into the empty result. With the result
 * alias every later consumer uses a value the producer defines, not a
 * synthetic one TPDE might schedule after a short-circuit PHI. The
 * producer's single consumer keeps its original polarity unobservable.
 */
static void freeze_inverted_empty_results(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &producer = plan->instructions[index];
		const zend_mir_instruction_record producer_record =
			zend_tpde_instruction_record_at(plan, &producer);
		zend_tpde_slot_isset_empty layout{};
		if (!producer.has_value_operation
				|| producer_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				|| (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0
				|| !zend_tpde_slot_isset_empty_at(producer, &layout)
				|| !layout.is_empty) {
			continue;
		}
		const int32_t input_index = machine_source_binding_value_index(
			plan, producer.source_op1_binding);
		const int32_t producer_result = machine_source_binding_value_index(
			plan, producer.source_result_binding);
		if (input_index < 0
				|| (plan->values[static_cast<uint32_t>(input_index)].exact_type
						!= ZEND_MIR_SCALAR_TYPE_I1
					&& plan->values[static_cast<uint32_t>(input_index)].exact_type
						!= ZEND_MIR_SCALAR_TYPE_I64)) {
			continue;
		}
		const int32_t unary_index =
			machine_single_source_consumer(plan, producer_result);
		if (unary_index <= static_cast<int32_t>(index)
				|| static_cast<uint32_t>(unary_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &unary =
			plan->instructions[static_cast<uint32_t>(unary_index)];
		const zend_mir_instruction_record unary_record =
			zend_tpde_instruction_record_at(plan, &unary);
		if (!unary.has_value_operation
				|| unary_record.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
				|| unary.value_operation.source_opcode != ZEND_BOOL_NOT
				|| unary_record.block_id != producer_record.block_id
				|| (unary.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0) {
			continue;
		}
		const int32_t unary_result = machine_source_binding_value_index(
			plan, unary.source_result_binding);
		const int32_t branch_index =
			machine_single_source_consumer(plan, unary_result);
		if (branch_index <= unary_index
				|| static_cast<uint32_t>(branch_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &branch =
			plan->instructions[static_cast<uint32_t>(branch_index)];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| branch_record.block_id != unary_record.block_id
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)) {
			continue;
		}
		producer.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_INVERT_RESULT;
		unary.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS;
		branch.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
	}
}

/*
 * The adaptor recognizes a two-edge short-circuit PHI over machine
 * booleans. Only its terminal branch is frozen here; intervening BOOL
 * operations stay definitions so edge values keep their polarity and
 * liveness.
 */
static void freeze_register_boolean_phis(
		zend_tpde_plan *plan,
		const std::vector<uint8_t> &register_boolean_results) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &phi = plan->instructions[index];
		const zend_mir_instruction_record phi_record =
			zend_tpde_instruction_record_at(plan, &phi);
		if (phi_record.opcode != ZEND_MIR_OPCODE_PHI
				|| phi.operand_count != 2) {
			continue;
		}
		bool exact_register_inputs = true;
		bool short_circuit_edge = false;
		for (uint32_t operand = 0; operand < phi.operand_count; ++operand) {
			const int32_t value_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &phi, operand));
			const int32_t producer_index =
				register_boolean_value_definition(plan, value_index);
			if (producer_index < 0
					|| static_cast<uint32_t>(producer_index)
						>= plan->instruction_count) {
				exact_register_inputs = false;
				break;
			}
			const zend_tpde_instruction &producer =
				plan->instructions[static_cast<uint32_t>(producer_index)];
			const zend_mir_instruction_record producer_record =
				zend_tpde_instruction_record_at(plan, &producer);
			const bool register_result =
				(producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
				&& register_boolean_results[
					static_cast<uint32_t>(producer_index)] != 0;
			const bool register_edge = producer.has_value_operation
				&& producer_record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& (producer.value_operation.source_opcode == ZEND_JMPZ_EX
					|| producer.value_operation.source_opcode == ZEND_JMPNZ_EX)
				&& (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
			if (!register_result && !register_edge) {
				exact_register_inputs = false;
				break;
			}
			short_circuit_edge = short_circuit_edge || register_edge;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan, phi_record.result_id);
		const int32_t branch_index = exact_register_inputs
			&& short_circuit_edge
			? machine_single_source_consumer(plan, result_index) : -1;
		if (branch_index < 0
				|| static_cast<uint32_t>(branch_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &branch =
			plan->instructions[static_cast<uint32_t>(branch_index)];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)) {
			continue;
		}
		phi.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT;
		branch.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
	}
}

/* The instructions of each block in order, built on first use. */
struct block_instruction_index {
	std::vector<uint32_t> offsets;
	std::vector<uint32_t> instructions;
};

/* The block's last instruction that writes the storage, or -1. */
static int32_t last_block_writer(
		const zend_tpde_plan *plan,
		block_instruction_index &blocks,
		uint32_t block, zend_mir_storage_id storage) {
	std::vector<uint32_t> &block_instruction_offsets = blocks.offsets;
	std::vector<uint32_t> &block_instructions = blocks.instructions;
	if (block_instruction_offsets.empty()) {
		std::vector<int32_t> instruction_blocks(plan->instruction_count);
		block_instruction_offsets.assign(plan->block_count + 1, 0);
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			instruction_blocks[i] = zend_tpde_block_index(plan,
				zend_tpde_instruction_record_at(
					plan, &plan->instructions[i]).block_id);
			if (instruction_blocks[i] >= 0) {
				++block_instruction_offsets[
					static_cast<uint32_t>(instruction_blocks[i]) + 1];
			}
		}
		for (uint32_t b = 0; b < plan->block_count; ++b) {
			block_instruction_offsets[b + 1] +=
				block_instruction_offsets[b];
		}
		block_instructions.resize(
			block_instruction_offsets[plan->block_count]);
		std::vector<uint32_t> fill(block_instruction_offsets.begin(),
			block_instruction_offsets.end() - 1);
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			if (instruction_blocks[i] >= 0) {
				block_instructions[fill[static_cast<uint32_t>(
					instruction_blocks[i])]++] = i;
			}
		}
	}
	for (uint32_t offset = block_instruction_offsets[block + 1];
			offset-- > block_instruction_offsets[block];) {
		const zend_tpde_instruction &writer =
			plan->instructions[block_instructions[offset]];
		if (writer.has_value_operation
				&& writer.value_operation.result_storage_id == storage) {
			return static_cast<int32_t>(block_instructions[offset]);
		}
	}
	return int32_t{-1};
}

/*
 * A Zend short-circuit temporary is a source value, not a MIR value.
 * When both definitions reaching the branch are machine booleans, the
 * branch becomes an implicit two-edge register merge: the adaptor builds
 * the TPDE PHI and the machine CFG omits the boxed branch diamond. The
 * definition from a predecessor is that block's last writer of the
 * temporary.
 */
static void freeze_register_boolean_merges(
		zend_tpde_plan *plan,
		const std::vector<uint8_t> &register_boolean_results) {
	block_instruction_index blocks;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &branch = plan->instructions[index];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		const int32_t condition_index = machine_source_binding_value_index(
			plan, branch.source_op1_binding);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)
				|| (branch.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) == 0
				|| condition_index < 0
				|| register_boolean_value_definition(
					plan, condition_index) >= 0
				|| !zend_mir_id_is_valid(
					branch.value_operation.op1_storage_id)
				|| plan->block_predecessor_offsets == nullptr
				|| plan->block_predecessors == nullptr) {
			continue;
		}
		const int32_t branch_block =
			zend_tpde_block_index(plan, branch_record.block_id);
		if (branch_block < 0) {
			continue;
		}
		const uint32_t predecessor_begin =
			plan->block_predecessor_offsets[
				static_cast<uint32_t>(branch_block)];
		const uint32_t predecessor_end =
			plan->block_predecessor_offsets[
				static_cast<uint32_t>(branch_block) + 1];
		if (predecessor_end - predecessor_begin != 2) {
			continue;
		}
		bool supported = true;
		bool short_circuit_edge = false;
		uint32_t producers[2]{};
		for (uint32_t predecessor_offset = predecessor_begin;
				predecessor_offset < predecessor_end;
				++predecessor_offset) {
			const int32_t candidate = last_block_writer(plan, blocks,
				plan->block_predecessors[predecessor_offset],
				branch.value_operation.op1_storage_id);
			if (candidate < 0) {
				supported = false;
				break;
			}
			const zend_tpde_instruction &producer =
				plan->instructions[static_cast<uint32_t>(candidate)];
			const zend_mir_instruction_record producer_record =
				zend_tpde_instruction_record_at(plan, &producer);
			const bool register_edge =
				producer_record.opcode
						== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& (producer.value_operation.source_opcode
						== ZEND_JMPZ_EX
					|| producer.value_operation.source_opcode
						== ZEND_JMPNZ_EX)
				&& (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH)
					!= 0;
			const bool register_result =
				(producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT)
					!= 0
				&& register_boolean_results[
					static_cast<uint32_t>(candidate)] != 0;
			if (!register_edge && !register_result) {
				supported = false;
				break;
			}
			producers[predecessor_offset - predecessor_begin] =
				static_cast<uint32_t>(candidate) + 1;
			short_circuit_edge = short_circuit_edge || register_edge;
		}
		if (supported && short_circuit_edge) {
			branch.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE;
			branch.register_merge_producer_plus_one[0] = producers[0];
			branch.register_merge_producer_plus_one[1] = producers[1];
		}
	}
}

void freeze_register_boolean_results(zend_tpde_plan *plan)
{
	if (plan->value_consumer_offsets == nullptr) {
		return;
	}
	std::vector<uint8_t> register_boolean_results(
		plan->instruction_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		register_boolean_results[index] =
			(plan->instructions[index].machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0;
	}
	freeze_register_short_circuit_branches(plan);
	freeze_register_isset_empty_results(plan, register_boolean_results);
	freeze_register_bool_chains(plan, register_boolean_results);
	freeze_inverted_empty_results(plan);
	freeze_register_boolean_phis(plan, register_boolean_results);
	freeze_register_boolean_merges(plan, register_boolean_results);
}

/* A keyed assignment whose key may be an integer and is never a string
 * (by Zend's type inference) into a container that may be an array; Linux
 * x64 lowers it inline and checks the key's type at run time. */
static bool machine_cfg_integer_key_assignment(
		const zend_tpde_plan *plan,
		const zend_mir_executable_value_ref &operation) {
	if (!zend_tpde_linux_inline_forms || plan->source_opcodes == nullptr
			|| operation.source_position_id >= plan->source_opcode_count
			|| (operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_SSA)) {
		return false;
	}
	const zend_tpde_source_opcode &source =
		plan->source_opcodes[operation.source_position_id];
	return source.op2_may_be != UINT32_MAX
		&& (source.op2_may_be & MAY_BE_LONG) != 0
		&& (source.op2_may_be & MAY_BE_STRING) == 0
		&& (source.op1_may_be == UINT32_MAX
			|| (source.op1_may_be & MAY_BE_ARRAY) != 0);
}

/* A keyed assignment under a string literal with its hash, or a key slot
 * that may be a string, into a container that may be an array. */
static bool machine_cfg_string_key_assignment(
		const zend_tpde_plan *plan,
		const zend_mir_executable_value_ref &operation) {
	if (!zend_tpde_linux_inline_forms || plan->source_opcodes == nullptr
			|| operation.source_position_id >= plan->source_opcode_count) {
		return false;
	}
	const zend_tpde_source_opcode &source =
		plan->source_opcodes[operation.source_position_id];
	if (source.op1_may_be != UINT32_MAX
			&& (source.op1_may_be & MAY_BE_ARRAY) == 0) {
		return false;
	}
	if (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (plan->source_literals == nullptr
				|| operation.op2.index >= plan->source_literal_count) {
			return false;
		}
		const zval *literal = &plan->source_literals[operation.op2.index];
		return Z_TYPE_P(literal) == IS_STRING
			&& ZSTR_H(Z_STR_P(literal)) != 0;
	}
	return (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
			|| operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
		&& (source.op2_may_be == UINT32_MAX
			|| (source.op2_may_be & (MAY_BE_STRING | MAY_BE_LONG)) != 0);
}

void freeze_machine_control_flow(zend_tpde_plan *plan)
{
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record &record = instruction.record;
		uint8_t flags = 0;

		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			if (instruction.runtime_helper
					== ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW) {
				flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			}
		} else if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			if (instruction.direct_call != nullptr
					&& (instruction.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0) {
				flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			}
			if (instruction.component_target_index != UINT32_MAX) {
				flags |=
					ZEND_TPDE_MACHINE_CONTROL_FLOW_TYPED_COMPONENT_CALL;
			}
		} else if (instruction.has_value_operation) {
			const zend_mir_executable_value_ref &operation =
				instruction.value_operation;
			switch (operation.opcode) {
				case ZEND_MIR_OPCODE_VALUE_ASSIGN:
					if (operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
						/* A string's last owner is freed inline. */
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW);
					}
					break;
				case ZEND_MIR_OPCODE_VALUE_UNARY_OP: {
					flags |=
						ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					break;
				}
				case ZEND_MIR_OPCODE_OBJECT_FETCH_IS:
				case ZEND_MIR_OPCODE_OBJECT_FETCH_W:
				case ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG:
					/* The Linux backend reads or addresses a cached
					 * declared property inline
					 * (zend_tpde_object_property_isset_read_at,
					 * zend_tpde_object_property_write_fetch_at,
					 * zend_tpde_object_property_func_arg_read_at). */
					if (zend_tpde_linux_inline_forms) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					}
					break;
				case ZEND_MIR_OPCODE_VALUE_TYPE_CHECK:
					/* A check of an exact scalar is a typed operation. */
					if (!machine_plan_type_check_supported(
							plan, instruction)) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					}
					break;
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
				case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
					/* A probe that cannot decide a key looks it up in C
					 * before it takes the helper. */
					flags |=
						ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_ARRAY_FIND_ELEMENT);
					break;
				case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
				case ZEND_MIR_OPCODE_VALUE_FREE:
				case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
				case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
				case ZEND_MIR_OPCODE_VALUE_INCDEC:
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
				case ZEND_MIR_OPCODE_VALUE_COUNT:
				case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
				case ZEND_MIR_OPCODE_VALUE_FE_FREE:
				case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
				case ZEND_MIR_OPCODE_OBJECT_FETCH_R:
				case ZEND_MIR_OPCODE_OBJECT_ASSIGN:
				case ZEND_MIR_OPCODE_DYNAMIC_FETCH_R:
					flags |=
						ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					break;
				case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
					/* An append, and the replacement or insertion of an
					 * element under an integer or string key, have inline
					 * forms; a missing key inserts out of line. */
					if (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					} else if (machine_cfg_integer_key_assignment(
							plan, operation)) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ARRAY_INSERT_INDEX);
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ARRAY_ASSIGN_LOOKUP);
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW);
					} else if (machine_cfg_string_key_assignment(
							plan, operation)) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ARRAY_ASSIGN_LOOKUP);
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW);
					}
					break;
				default:
					break;
			}
		}

		const bool branch_shape =
			instruction.has_value_operation
			&& (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH)
			&& (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				? instruction.value_operation.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				: instruction.value_operation.opcode == record.opcode);
		if (branch_shape) {
			flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH;
			const bool follows_boolean_unary =
				record.source_position_id > 0
				&& record.source_position_id < plan->source_opcode_count
				&& plan->source_opcodes != nullptr
				&& (plan->source_opcodes[
						record.source_position_id - 1].opcode == ZEND_BOOL
					|| plan->source_opcodes[
						record.source_position_id - 1].opcode == ZEND_BOOL_NOT);
			if ((record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH)
					&& instruction.value_operation.opcode
						== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& !follows_boolean_unary
			&& (instruction.value_operation.source_opcode
						== ZEND_JMPZ
					|| instruction.value_operation.source_opcode
						== ZEND_JMPNZ)) {
				flags |=
					ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
			}
		}
		instruction.machine_control_flow_flags = flags;
	}
}

} // namespace zend::native::tpde::plan_detail
