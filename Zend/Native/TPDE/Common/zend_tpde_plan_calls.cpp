// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

bool call_site_requires_source_fragments(
	const zend_tpde_plan *plan, const zend_mir_call_site_ref &site)
{
	uint32_t source_position;

	if (plan->source_opcodes == nullptr
			|| site.source_init_opline_index >= plan->source_opcode_count
			|| site.source_do_opline_index >= plan->source_opcode_count
			|| site.source_init_opline_index
				>= site.source_do_opline_index) {
		return false;
	}
	if (zend_get_user_opcode_handler(
			plan->source_opcodes[
				site.source_init_opline_index].opcode) != nullptr
			|| zend_get_user_opcode_handler(
				plan->source_opcodes[
					site.source_do_opline_index].opcode) != nullptr) {
		return true;
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
				plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= plan->source_opcode_count) {
			return false;
		}
		if (zend_get_user_opcode_handler(
				plan->source_opcodes[
					argument.send_opline_index].opcode) != nullptr) {
			return true;
		}
	}
	/*
	 * CHECK_FUNC_ARG and CHECK_UNDEF_ARGS observe the pending call frame at
	 * their source positions, so a call with either keeps its INIT/SEND/DO
	 * fragments: FETCH_*_FUNC_ARG must see the callee's by-reference
	 * contract before it fetches its lvalue.
	 */
	for (source_position = site.source_init_opline_index + 1;
			source_position < site.source_do_opline_index;
		source_position++) {
		const uint8_t opcode =
			plan->source_opcodes[source_position].opcode;
		if (opcode == ZEND_CHECK_FUNC_ARG
				|| opcode == ZEND_CHECK_UNDEF_ARGS) {
			return true;
		}
	}
	return false;
}

bool call_site_participates_in_nested_call(
	const zend_mir_call_view *calls, const zend_mir_call_site_ref &site)
{
	if (calls == nullptr) {
		return false;
	}
	const uint32_t call_site_count = calls->call_site_count(calls->context);
	for (uint32_t index = 0; index < call_site_count; ++index) {
		zend_mir_call_site_ref candidate{};

		if (!calls->call_site_at(calls->context, index, &candidate)) {
			continue;
		}
		if (candidate.id != site.id
				&& ((candidate.source_init_opline_index
						> site.source_init_opline_index
					&& candidate.source_do_opline_index
						< site.source_do_opline_index)
					|| (site.source_init_opline_index
						> candidate.source_init_opline_index
					&& site.source_do_opline_index
						< candidate.source_do_opline_index))) {
			return true;
		}
	}
	return false;
}

/*
 * A nested call keeps VM-ordered INIT/SEND/DO fragments unless every call of
 * its overlapping group can transfer its arguments at DO instead. A call
 * qualifies when it statically targets a user function, or an internal
 * function bound without scope (INIT_FCALL/DO_ICALL), with by-value
 * arguments whose slots nothing touches before its DO. An argument sent
 * before an inner call completes must also be one that call cannot change,
 * so not a reference. If the inner call throws, the exception cleanup
 * releases the arguments already sent (see
 * zend_native_cleanup_unfinished_exception()).
 */
bool nested_call_site_direct_candidate(
	const zend_tpde_plan *plan, const zend_mir_call_view *calls,
	const zend_native_call_binding *user_bindings,
	const zend_native_internal_call_binding *internal_bindings,
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_call_site_ref &site)
{
	if (op_array == nullptr || ssa == nullptr || ssa->var_info == nullptr
			|| ssa->ops == nullptr || op_array->function_name == nullptr
			|| calls == nullptr
			|| site.source_init_opline_index >= op_array->last
			|| site.source_do_opline_index >= op_array->last
			|| op_array->opcodes[site.source_init_opline_index].opcode
				!= ZEND_INIT_FCALL
			|| call_site_requires_source_fragments(plan, site)) {
		return false;
	}
	const uint8_t do_opcode =
		op_array->opcodes[site.source_do_opline_index].opcode;
	const int32_t target_index = id_index_find(plan->call_target_index,
		plan->call_target_index_capacity, site.target_id);
	zend_mir_call_target_ref target{};
	if (target_index < 0
			|| !calls->call_target_at(calls->context,
				static_cast<uint32_t>(target_index), &target)) {
		return false;
	}
	const zend_function *callee = nullptr;
	if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_USER) {
		const int32_t binding_index = id_index_find(plan->user_binding_index,
			plan->user_binding_index_capacity, site.target_id);
		callee = binding_index >= 0
				&& user_bindings[binding_index].entry_cell != nullptr
			? user_bindings[binding_index].entry_cell->function : nullptr;
		if (callee == nullptr || callee->type != ZEND_USER_FUNCTION
				|| (do_opcode != ZEND_DO_UCALL && do_opcode != ZEND_DO_FCALL)) {
			return false;
		}
	} else if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
			&& internal_bindings != nullptr) {
		const int32_t binding_index = id_index_find(
			plan->internal_binding_index,
			plan->internal_binding_index_capacity, site.target_id);
		callee = binding_index >= 0
				&& internal_bindings[binding_index].call_cell != nullptr
			? internal_bindings[binding_index].call_cell->function : nullptr;
		/* A declared exact scalar result becomes a register value of the
		 * call, which the direct group does not transport. */
		if (callee == nullptr || callee->type != ZEND_INTERNAL_FUNCTION
				|| callee->common.scope != nullptr
				|| do_opcode != ZEND_DO_ICALL
				|| ((callee->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) != 0
					&& callee->common.arg_info != nullptr
					&& zend_mir_scalar_type_is_exact(
						exact_scalar_from_declared_type(
							callee->common.arg_info[-1].type)))) {
			return false;
		}
	} else {
		return false;
	}
	if (site.arguments.count < callee->common.required_num_args) {
		return false;
	}
	for (uint32_t n = 0; n < site.arguments.count; ++n) {
		zend_mir_call_argument_ref argument{};
		if (!zend_tpde_call_argument_at(
				plan, site.arguments.offset + n, &argument)
				|| argument.source_mode
					!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
				|| argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
				|| argument.send_opline_index
					>= site.source_do_opline_index
				|| ARG_SHOULD_BE_SENT_BY_REF(callee, argument.ordinal + 1)) {
			return false;
		}
		const zend_op *send = &op_array->opcodes[argument.send_opline_index];
		if (send->opcode != ZEND_SEND_VAL && send->opcode != ZEND_SEND_VAR) {
			return false;
		}
		if (argument.source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			continue;
		}
		if ((argument.source_operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& argument.source_operand.kind
						!= ZEND_MIR_SOURCE_OPERAND_SSA)
				|| source_call_argument_may_be_undefined(
					op_array, ssa, argument)
				|| !source_call_argument_stable_after_send(
					op_array, argument, site.source_do_opline_index)) {
			return false;
		}
		bool before_inner_call = false;
		for (uint32_t position = argument.send_opline_index + 1;
				position < site.source_do_opline_index; ++position) {
			const uint8_t opcode = op_array->opcodes[position].opcode;
			if (opcode == ZEND_DO_FCALL || opcode == ZEND_DO_ICALL
					|| opcode == ZEND_DO_UCALL
					|| opcode == ZEND_DO_FCALL_BY_NAME
					|| opcode == ZEND_CALLABLE_CONVERT
					|| opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
				before_inner_call = true;
				break;
			}
		}
		if (!before_inner_call) {
			continue;
		}
		const int ssa_variable =
			ssa->ops[argument.send_opline_index].op1_use;
		if (ssa_variable < 0 || ssa_variable >= ssa->vars_count) {
			return false;
		}
		if ((ssa->var_info[ssa_variable].type & MAY_BE_REF) != 0) {
			return false;
		}
	}
	return true;
}

zend_native_user_call_descriptor *build_user_call_descriptor(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_call_site_ref &site,
	const zend_mir_instruction_record &record,
	zend_native_diagnostic *diag)
{
	if (source_op_array == nullptr
			|| site.source_init_opline_index >= source_op_array->last
			|| site.source_do_opline_index >= source_op_array->last) {
		return nullptr;
	}
	const zend_op *init = &source_op_array->opcodes[
		site.source_init_opline_index];
	const zend_op *finish = &source_op_array->opcodes[
		site.source_do_opline_index];
	if (init->extended_value > site.arguments.count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"call source argument count is inconsistent");
		return nullptr;
	}
	const size_t descriptor_size =
		offsetof(zend_native_user_call_descriptor, arguments)
		+ static_cast<size_t>(site.arguments.count)
			* sizeof(zend_native_direct_internal_call_argument);
	auto *descriptor =
		zend_native_user_call_descriptor_alloc(descriptor_size);
	if (descriptor == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a source call descriptor");
		return nullptr;
	}
	descriptor->argument_count = site.arguments.count;
	descriptor->initial_argument_count = init->extended_value;
	descriptor->init_source_position = site.source_init_opline_index;
	descriptor->do_source_position = site.source_do_opline_index;
	descriptor->init_opcode = init->opcode;
	descriptor->do_opcode = finish->opcode;
	descriptor->init_op1_payload = init->op1.num;
	descriptor->init_op2_payload = init->op2.num;
	descriptor->init_result_payload = init->result.num;
	descriptor->init_extended_value = init->extended_value;
	descriptor->do_op1_payload = finish->op1.num;
	descriptor->do_op2_payload = finish->op2.num;
	descriptor->do_result_payload = finish->result.num;
	descriptor->do_extended_value = finish->extended_value;
	if (!source_descriptor_operand(
				source_op_array, init, init->op1_type,
				init->op1, &descriptor->init_op1)
			|| !source_descriptor_operand(
				source_op_array, init, init->op2_type,
				init->op2, &descriptor->init_op2)
			|| !source_descriptor_operand(
				source_op_array, init, init->result_type,
				init->result, &descriptor->init_result)
			|| !source_descriptor_operand(
				source_op_array, finish, finish->op1_type,
				finish->op1, &descriptor->do_op1)
			|| !source_descriptor_operand(
				source_op_array, finish, finish->op2_type,
				finish->op2, &descriptor->do_op2)) {
		zend_native_user_call_descriptor_free(descriptor);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"call source operands are invalid");
		return nullptr;
	}
	descriptor->do_result = site.result_operand;
	if (finish->opcode != ZEND_CALLABLE_CONVERT
			&& finish->opcode != ZEND_CALLABLE_CONVERT_PARTIAL
			&& zend_mir_id_is_valid(record.result_id)) {
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0) {
			zend_native_user_call_descriptor_free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call result is unknown");
			return nullptr;
		}
		if (zend_mir_scalar_type_is_exact(
				plan->values[result_index].exact_type)) {
			descriptor->result_type =
				plan->values[result_index].exact_type;
			descriptor->flags |=
				ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT;
		}
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
					plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= source_op_array->last) {
			zend_native_user_call_descriptor_free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call argument table is unreadable");
			return nullptr;
		}
		const zend_op *send = &source_op_array->opcodes[
			argument.send_opline_index];
		if (!source_descriptor_send_opcode(send->opcode)) {
			zend_native_user_call_descriptor_free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call SEND opcode is invalid");
			return nullptr;
		}
		zend_native_direct_internal_call_argument &encoded =
			descriptor->arguments[index];
		encoded.ordinal = argument.ordinal;
		encoded.mode = argument.source_mode
				== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
			? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
			: argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
				? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
				: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
		encoded.source_opcode = send->opcode;
		encoded.source_position = argument.send_opline_index;
		encoded.source_operand = argument.source_operand;
		encoded.auxiliary_payload = send->op2.num;
		encoded.result_payload = send->result.num;
		encoded.extended_value = send->extended_value;
		if (!source_descriptor_operand(
				source_op_array, send, send->op2_type,
				send->op2, &encoded.auxiliary_operand)) {
			zend_native_user_call_descriptor_free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call auxiliary operand is invalid");
			return nullptr;
		}
	}
	return descriptor;
}

bool freeze_source_call_phases(
	zend_tpde_plan *plan, zend_native_diagnostic *diag)
{
	if (plan->source_opcode_count == 0
			|| plan->source_opcodes == nullptr) {
		return true;
	}

	auto phase_candidate = [&](uint32_t instruction_index) {
		if (instruction_index >= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& instruction.user_call != nullptr
			&& instruction.direct_call == nullptr
			&& instruction.user_call->do_opcode != ZEND_CALLABLE_CONVERT
			&& instruction.user_call->do_opcode
				!= ZEND_CALLABLE_CONVERT_PARTIAL;
	};

	bool any_candidate = false;
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		any_candidate = any_candidate || phase_candidate(instruction_index);
	}
	if (!any_candidate) {
		return true;
	}

	plan->source_call_phases =
		static_cast<zend_tpde_source_call_phase_entry *>(std::calloc(
			plan->source_opcode_count, sizeof(*plan->source_call_phases)));
	if (plan->source_call_phases == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate source-call phase table");
		return false;
	}
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		plan->source_call_phases[source].instruction_index = UINT32_MAX;
		plan->source_call_phases[source].argument_index = UINT32_MAX;
		plan->source_call_phases[source].value_index = -1;
	}

	std::vector<uint32_t> init_events(plan->source_opcode_count, UINT32_MAX);
	std::vector<uint32_t> do_events(plan->source_opcode_count, UINT32_MAX);
	auto assign_event = [&](std::vector<uint32_t> &events,
			uint32_t source, uint32_t instruction_index) {
		if (source >= events.size() || events[source] != UINT32_MAX) {
			return false;
		}
		events[source] = instruction_index;
		return true;
	};
	auto assign_phase = [&](uint32_t source, uint32_t instruction_index,
			uint8_t phases) -> zend_tpde_source_call_phase_entry * {
		if (source >= plan->source_opcode_count) {
			return nullptr;
		}
		zend_tpde_source_call_phase_entry &entry =
			plan->source_call_phases[source];
		if (entry.instruction_index != UINT32_MAX
				&& entry.instruction_index != instruction_index) {
			return nullptr;
		}
		entry.instruction_index = instruction_index;
		entry.phases |= phases;
		return &entry;
	};

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
			continue;
		}
		const zend_mir_call_site_ref &site = *instruction.call_site;
		if (site.source_init_opline_index >= site.source_do_opline_index
				|| !assign_event(init_events,
					site.source_init_opline_index, instruction_index)
				|| !assign_event(do_events,
					site.source_do_opline_index, instruction_index)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call intervals are invalid or overlap at an endpoint");
			return false;
		}
		if (!phase_candidate(instruction_index)) {
			continue;
		}
		if (assign_phase(site.source_init_opline_index, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_INIT) == nullptr
				|| assign_phase(site.source_do_opline_index, instruction_index,
					ZEND_TPDE_SOURCE_CALL_PHASE_DO) == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call phases collide at an INIT or DO opline");
			return false;
		}

		for (uint32_t argument = 0;
				argument < instruction.call_argument_count; ++argument) {
			const uint32_t argument_index =
				instruction.call_argument_offset + argument;
			zend_mir_call_argument_ref source_argument{};
			if (!zend_tpde_call_argument_at(
					plan, argument_index, &source_argument)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call argument phase is unreadable");
				return false;
			}
			zend_tpde_source_call_phase_entry *entry = assign_phase(
				source_argument.send_opline_index, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_SEND);
			if (entry == nullptr || entry->argument_index != UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call SEND phases are duplicated or collide");
				return false;
			}
			/* Placement indices are descriptor-local, never plan-global. */
			entry->argument_index = argument;

			const zend_tpde_source_value_binding &binding =
				plan->call_argument_bindings[argument_index];
			bool direct_value = binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan->value_count
				&& (source_argument.source_mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					|| source_argument.source_mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED)
				&& source_argument.ownership
					!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			if (direct_value) {
				const zend_tpde_value &value = plan->values[
					static_cast<uint32_t>(binding.value_index)];
				const zend_mir_scalar_type_mask exact_type = value.exact_type;
				/* A value the plan keeps as a zval is sent from its zval. */
				direct_value = (exact_type == ZEND_MIR_SCALAR_TYPE_I1
						|| exact_type == ZEND_MIR_SCALAR_TYPE_I64
						|| exact_type == ZEND_MIR_SCALAR_TYPE_F64)
					&& value.machine_kind
						!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& (binding.definition_instruction_index >= 0
						|| value.constant || value.argument_index >= 0);
			}
			if (direct_value && binding.definition_instruction_index >= 0) {
				const uint32_t definition_index = static_cast<uint32_t>(
					binding.definition_instruction_index);
				if (definition_index >= plan->instruction_count) {
					direct_value = false;
				} else {
					const zend_mir_instruction_record definition =
						zend_tpde_instruction_record_at(
							plan, &plan->instructions[definition_index]);
					direct_value = definition.source_position_id
						< source_argument.send_opline_index
						&& plan->source_opcode_block_indices != nullptr
						&& definition.source_position_id
							< plan->source_opcode_count
						&& plan->source_opcode_block_indices[
							definition.source_position_id]
							== plan->source_opcode_block_indices[
								source_argument.send_opline_index];
				}
			}
			if (direct_value) {
				entry->value_index = binding.value_index;
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE;
			} else {
				const zend_mir_source_operand_ref &operand =
					source_argument.source_operand;
				const bool source_operand =
					operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| (operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA
						&& operand.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_KIND_INVALID)
					|| (operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& source_argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER);
				if (!source_operand) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source-call SEND value is defined after its landing");
					return false;
				}
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_SOURCE;
			}

			if (source_argument.source_mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK) {
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_RUNTIME_EXPANSION;
			}
		}
		/*
		 * The expansion node is a conditional statepoint: a no-op for a fully
		 * direct placement, it calls the helper only for an unpack or an
		 * extra named tail that resolution left to run time.
		 */
		if (assign_phase(site.source_do_opline_index, instruction_index,
					ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND) == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call expansion phase collides at its DO opline");
			return false;
		}
	}

	std::vector<uint32_t> active_calls;
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		if (init_events[source] != UINT32_MAX) {
			active_calls.push_back(init_events[source]);
		}
		const uint8_t opcode = plan->source_opcodes[source].opcode;
		if ((opcode == ZEND_CHECK_FUNC_ARG
				|| opcode == ZEND_CHECK_UNDEF_ARGS)
				&& !active_calls.empty()
				&& phase_candidate(active_calls.back())) {
			const uint32_t instruction_index = active_calls.back();
			zend_tpde_source_call_phase_entry *entry = assign_phase(
				source, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_CHECK);
			if (entry == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call CHECK phase collides with another call");
				return false;
			}
			if (opcode == ZEND_CHECK_FUNC_ARG) {
				const zend_tpde_instruction &instruction =
					plan->instructions[instruction_index];
				uint32_t next_argument = UINT32_MAX;
				uint32_t next_send = UINT32_MAX;
				for (uint32_t argument = 0;
						argument < instruction.call_argument_count;
						++argument) {
					zend_mir_call_argument_ref candidate{};
					if (!zend_tpde_call_argument_at(plan,
							instruction.call_argument_offset + argument,
							&candidate)) {
						next_argument = UINT32_MAX;
						break;
					}
					if (candidate.send_opline_index > source
							&& candidate.send_opline_index < next_send) {
						next_send = candidate.send_opline_index;
						next_argument = argument;
					}
				}
				if (next_argument == UINT32_MAX) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"CHECK_FUNC_ARG has no following placement");
					return false;
				}
				entry->argument_index = next_argument;
			}
		}
		if (do_events[source] != UINT32_MAX) {
			if (active_calls.empty()
					|| active_calls.back() != do_events[source]) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call intervals are not correctly nested");
				return false;
			}
			active_calls.pop_back();
		}
	}
	if (!active_calls.empty()) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source-call interval remains open at function end");
		return false;
	}

	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		plan->source_call_phase_count +=
			plan->source_call_phases[source].phases != 0;
	}
	return true;
}

/* A direct user call's frame, as its arguments shape it. */
struct direct_call_frame {
	std::vector<uint8_t> supplied_parameters;
	std::vector<zend_mir_scalar_type_mask> argument_guard_types;
	bool has_argument_guard_type = false;
	bool trivial_frame = false;
};

static bool site_ids_unique(plan_builder &builder) {
	const zend_mir_call_site_ref *const sites = builder.sites;
	const uint32_t site_count = builder.site_count;
	int8_t &unique_site_ids = builder.unique_site_ids;
	if (unique_site_ids < 0) {
		std::vector<zend_mir_call_site_id> ids(site_count);
		for (uint32_t index = 0; index < site_count; ++index) {
			ids[index] = sites[index].id;
		}
		std::ranges::sort(ids);
		unique_site_ids =
			std::adjacent_find(ids.begin(), ids.end()) == ids.end() ? 1 : 0;
	}
	return unique_site_ids == 1;
}

static bool participates_in_nested_call(
	plan_builder &builder, uint32_t site_index) {
	const zend_mir_call_view *const calls = builder.calls;
	const zend_mir_call_site_ref *const sites = builder.sites;
	const uint32_t site_count = builder.site_count;
	std::vector<uint8_t> &nested_participants = builder.nested_participants;
	if (!site_ids_unique(builder)) {
		return call_site_participates_in_nested_call(
			calls, sites[site_index]);
	}
	if (nested_participants.empty()) {
		nested_participants.assign(site_count, 0);
		std::vector<uint32_t> order(site_count);
		for (uint32_t index = 0; index < site_count; ++index) {
			order[index] = index;
		}
		std::ranges::sort(order, [&](uint32_t left, uint32_t right) {
			return sites[left].source_init_opline_index
				< sites[right].source_init_opline_index;
		});
		/* A site in another: one with a smaller INIT ends after it. */
		int64_t latest_do = -1;
		for (uint32_t begin = 0; begin < site_count;) {
			uint32_t end = begin;
			while (end < site_count
					&& sites[order[end]].source_init_opline_index
						== sites[order[begin]].source_init_opline_index) {
				++end;
			}
			for (uint32_t at = begin; at < end; ++at) {
				if (latest_do > sites[order[at]].source_do_opline_index) {
					nested_participants[order[at]] = 1;
				}
			}
			for (uint32_t at = begin; at < end; ++at) {
				latest_do = std::max<int64_t>(latest_do,
					sites[order[at]].source_do_opline_index);
			}
			begin = end;
		}
		/* Another in a site: one with a larger INIT ends before it. */
		int64_t earliest_do = INT64_MAX;
		for (uint32_t end = site_count; end > 0;) {
			uint32_t begin = end;
			while (begin > 0
					&& sites[order[begin - 1]].source_init_opline_index
						== sites[order[end - 1]].source_init_opline_index) {
				--begin;
			}
			for (uint32_t at = begin; at < end; ++at) {
				if (earliest_do < sites[order[at]].source_do_opline_index) {
					nested_participants[order[at]] = 1;
				}
			}
			for (uint32_t at = begin; at < end; ++at) {
				earliest_do = std::min<int64_t>(earliest_do,
					sites[order[at]].source_do_opline_index);
			}
			end = begin;
		}
	}
	return nested_participants[site_index] != 0;
}

static bool nested_call_group_direct(
	plan_builder &builder, uint32_t site_index) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_call_view *const calls = builder.calls;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	const zend_native_internal_call_binding *const internal_bindings =
		builder.internal_bindings;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	const zend_mir_call_site_ref *const sites = builder.sites;
	const uint32_t site_count = builder.site_count;
	std::vector<int8_t> &nested_direct_groups = builder.nested_direct_groups;
	if (!site_ids_unique(builder)) {
		/* The last site of the ID, as a search by ID finds it. */
		for (uint32_t index = 0; index < site_count; ++index) {
			if (sites[index].id == sites[site_index].id) {
				site_index = index;
			}
		}
	}
	if (nested_direct_groups.empty()) {
		nested_direct_groups.assign(site_count, -1);
	}
	if (nested_direct_groups[site_index] >= 0) {
		return nested_direct_groups[site_index] == 1;
	}
	auto overlap = [&](uint32_t left, uint32_t right) {
		const zend_mir_call_site_ref &a = sites[left];
		const zend_mir_call_site_ref &b = sites[right];
		return (a.source_init_opline_index > b.source_init_opline_index
				&& a.source_do_opline_index < b.source_do_opline_index)
			|| (b.source_init_opline_index > a.source_init_opline_index
				&& b.source_do_opline_index < a.source_do_opline_index);
	};
	std::vector<uint32_t> group{site_index};
	std::vector<uint8_t> in_group(site_count, 0);
	in_group[site_index] = 1;
	bool direct = true;
	for (size_t next = 0; next < group.size(); ++next) {
		direct = direct && nested_call_site_direct_candidate(plan, calls,
			user_bindings, internal_bindings, source_op_array, source_ssa,
			sites[group[next]]);
		for (uint32_t index = 0; index < site_count; ++index) {
			if (!in_group[index] && overlap(group[next], index)) {
				in_group[index] = 1;
				group.push_back(index);
			}
		}
	}
	for (uint32_t member : group) {
		nested_direct_groups[member] = direct ? 1 : 0;
	}
	return direct;
}

/*
 * The result of a call to a user function that does not return by
 * reference, sent by SEND_VAR, is a plain value: the send moves it like a
 * temporary, as f(g($x)) does.
 */
static bool call_result_argument_is_value(
	const plan_builder &builder, const zend_mir_call_argument_ref &argument) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_mir_call_site_ref *const sites = builder.sites;
	const uint32_t site_count = builder.site_count;
	if (source_op_array == nullptr
			|| argument.send_opline_index >= source_op_array->last) {
		return false;
	}
	const zend_op &send =
		source_op_array->opcodes[argument.send_opline_index];
	if (send.opcode != ZEND_SEND_VAR || send.op1_type != IS_VAR) {
		return false;
	}
	uint32_t producer = argument.send_opline_index;
	while (producer-- > 0) {
		const zend_op &candidate = source_op_array->opcodes[producer];
		if ((candidate.result_type & (IS_TMP_VAR | IS_VAR)) != 0
				&& candidate.result.var == send.op1.var) {
			break;
		}
	}
	if (producer >= argument.send_opline_index
			|| source_op_array->opcodes[producer].result_type != IS_VAR
			|| (source_op_array->opcodes[producer].opcode != ZEND_DO_UCALL
				&& source_op_array->opcodes[producer].opcode
					!= ZEND_DO_FCALL)) {
		return false;
	}
	for (uint32_t index = 0; index < site_count; ++index) {
		const zend_mir_call_site_ref &producer_site = sites[index];
		if (producer_site.source_do_opline_index != producer) {
			continue;
		}
		const int32_t binding_index = id_index_find(
			plan->user_binding_index,
			plan->user_binding_index_capacity, producer_site.target_id);
		const zend_function *callee = binding_index >= 0
				&& user_bindings[binding_index].entry_cell != nullptr
			? user_bindings[binding_index].entry_cell->function : nullptr;
		return callee != nullptr && callee->type == ZEND_USER_FUNCTION
			&& (callee->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) == 0;
	}
	return false;
}

static void freeze_numeric_variant_target(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site, int32_t binding_index) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	/* Entry specialization: exact integer arguments call the
	 * target's integer variant without the entry check. */
	if (user_bindings[binding_index].variant_numeric
			&& user_bindings[binding_index]
				.variant_component_index_plus_one != 0
			&& user_bindings[binding_index].component_target_index
				!= UINT32_MAX) {
		plan->instructions[i].numeric_variant_plus_one =
			user_bindings[binding_index]
				.variant_component_index_plus_one;
		plan->instructions[i].numeric_variant_general =
			user_bindings[binding_index].component_target_index;
		plan->instructions[i].numeric_variant_mask =
			user_bindings[binding_index].variant_long_mask;
	} else if (user_bindings[binding_index]
				.variant_component_index_plus_one != 0
			&& user_bindings[binding_index].component_target_index
				!= UINT32_MAX) {
		const uint32_t mask =
			user_bindings[binding_index].variant_long_mask;
		uint32_t proven = 0;
		for (uint32_t n = 0; n < site.arguments.count; ++n) {
			zend_mir_call_argument_ref argument{};
			if (!zend_tpde_call_argument_at(
					plan, site.arguments.offset + n, &argument)
					|| argument.ordinal >= 32
					|| ((mask >> argument.ordinal) & 1) == 0
					|| argument.source_mode
						!= ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE) {
				continue;
			}
			const int32_t value_index = zend_tpde_value_index(
				plan, argument.value_id);
			/* A source-backed argument is proven by the type
			 * Zend's inference gives its SEND. */
			const bool known_long = plan->source_opcodes != nullptr
				&& argument.send_opline_index
					< plan->source_opcode_count
				&& plan->source_opcodes[
					argument.send_opline_index].op1_known_type
					== IS_LONG;
			if (known_long || (value_index >= 0
					&& plan->values[value_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_I64)) {
				proven |= UINT32_C(1) << argument.ordinal;
			}
		}
		if (proven == mask) {
			plan->instructions[i].component_target_index =
				user_bindings[binding_index]
					.variant_component_index_plus_one - 1;
		}
	}
}

/* Whether a direct user call takes a direct descriptor, which transfers
 * its arguments at DO_FCALL. */
static bool direct_user_call_descriptor_eligible(const plan_builder &builder,
	const zend_mir_call_site_ref &site, const zend_mir_call_target_ref &target,
	int32_t binding_index, bool fragment_call,
	zend_function *expected_function) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	bool direct_descriptor =
		!fragment_call
		&& source_op_array->opcodes[
			site.source_do_opline_index].opcode
			!= ZEND_CALLABLE_CONVERT
		&& source_op_array->opcodes[
			site.source_do_opline_index].opcode
			!= ZEND_CALLABLE_CONVERT_PARTIAL
		&& (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_USER
			|| (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER
				&& user_bindings[binding_index].direct_native))
		&& site.arguments.count >= target.required_num_args;
	for (uint32_t n = 0;
			direct_descriptor && n < site.arguments.count; ++n) {
		const zend_mir_call_argument_ref &argument =
			plan->call_arguments[site.arguments.offset + n];
		direct_descriptor =
			(argument.source_mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
				|| argument.source_mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
				|| argument.source_mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED)
			&& (argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				|| argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| argument.source_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA);
		if (!direct_descriptor) {
			break;
		}
		uint32_t parameter_ordinal = argument.ordinal;
		if (!direct_call_parameter_ordinal(
				source_op_array, expected_function, argument,
				&parameter_ordinal)) {
			direct_descriptor = false;
			break;
		}
		const zend_op *send =
			argument.send_opline_index < source_op_array->last
				? &source_op_array->opcodes[
					argument.send_opline_index]
				: nullptr;
		const bool parameter_by_reference =
			expected_function != nullptr
			&& ARG_MUST_BE_SENT_BY_REF(
				expected_function, parameter_ordinal + 1);
		const bool reference_argument =
			parameter_by_reference
			|| argument.source_mode
				== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
			|| argument.ownership
				== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
			|| (send != nullptr && send->opcode == ZEND_SEND_REF);
		const bool argument_may_be_undefined =
			source_call_argument_may_be_undefined(
				source_op_array, source_ssa, argument);
		const bool direct_reference_cv =
			reference_argument
			&& !argument_may_be_undefined
			&& (argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& argument.source_operand.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV;
		const bool stable_by_value_source =
			reference_argument
			|| argument.source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| source_call_argument_stable_after_send(
				source_op_array, argument,
				site.source_do_opline_index);
		/*
		 * A direct descriptor transfers arguments at DO_FCALL.
		 * Defined CV references may observe intervening writes, but
		 * by-value arguments must keep their SEND-time value: reused
		 * storage, other references and possibly undefined reads
		 * keep their SEND timing and validation.
		 */
		if ((reference_argument && !direct_reference_cv)
				|| argument_may_be_undefined
				|| !stable_by_value_source) {
			direct_descriptor = false;
		}
	}
	return direct_descriptor;
}

static zend_native_direct_call_descriptor *create_direct_user_call_descriptor(
	const plan_builder &builder, uint32_t i, const zend_mir_call_site_ref &site,
	const zend_mir_call_target_ref &target, zend_function *expected_function,
	const zend_op_array *expected_op_array) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_op *init = nullptr;
	if (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER) {
		if (site.source_init_opline_index
				>= source_op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"direct native method has no source descriptor");
			return nullptr;
		}
		init = &source_op_array->opcodes[
			site.source_init_opline_index];
	}
	/*
	 * Keep one literal-map entry per fixed parameter.  UINT32_MAX
	 * marks a supplied slot.  A slot-indexed map is required for
	 * named calls, where omitted defaults are not necessarily a
	 * contiguous suffix of the source argument list.
	 */
	const uint32_t default_literal_count =
		expected_op_array != nullptr
			? expected_op_array->num_args : 0;
	const size_t descriptor_size =
		offsetof(zend_native_direct_call_descriptor, arguments)
		+ static_cast<size_t>(site.arguments.count)
			* sizeof(zend_native_direct_call_argument)
		+ static_cast<size_t>(default_literal_count)
			* sizeof(uint32_t);
	auto *descriptor =
		static_cast<zend_native_direct_call_descriptor *>(
			std::calloc(1, descriptor_size));
	if (descriptor == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a direct user-call descriptor");
		return nullptr;
	}
	descriptor->argument_count = site.arguments.count;
	descriptor->frame_argument_count = site.arguments.count;
	descriptor->default_literal_count =
		default_literal_count;
	for (uint32_t n = 0; n < default_literal_count; ++n) {
		zend_native_direct_call_default_literals(descriptor)[n] =
			UINT32_MAX;
	}
	descriptor->source_position =
		site.source_do_opline_index;
	descriptor->expected_function = expected_function;
	if (expected_op_array != nullptr) {
		descriptor->callee_argument_count =
			expected_op_array->num_args;
		descriptor->callee_compiled_variable_count =
			expected_op_array->last_var;
		descriptor->callee_temporary_count =
			expected_op_array->T;
	}
	if (plan->instructions[i].entry_cell != nullptr
			&& plan->instructions[i].entry_cell->lease_managed) {
		descriptor->flags |=
			ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED;
	}
	descriptor->receiver_kind =
		ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
	descriptor->receiver_operand.kind =
		ZEND_MIR_SOURCE_OPERAND_UNUSED;
	descriptor->receiver_operand.slot_kind =
		ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	descriptor->receiver_operand.index =
		ZEND_MIR_ID_INVALID;
	descriptor->receiver_operand.ssa_variable_id =
		ZEND_MIR_ID_INVALID;
	descriptor->receiver_source_frame_offset = UINT32_MAX;
	if (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER) {
		if (init->opcode == ZEND_INIT_STATIC_METHOD_CALL) {
			if (init->op2_type != IS_CONST
					|| descriptor->expected_function == nullptr
					|| descriptor->expected_function->common.scope
						== nullptr
					|| (descriptor->expected_function->common.fn_flags
						& ZEND_ACC_STATIC) == 0
					|| (init->op1_type != IS_CONST
						&& init->op1_type != IS_UNUSED)) {
				std::free(descriptor);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct native static method has no fixed scope");
				return nullptr;
			}
			descriptor->receiver_kind =
				ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE;
			if (init->op1_type == IS_UNUSED) {
				descriptor->flags |=
					ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE;
			} else {
				descriptor->called_scope =
					descriptor->expected_function->common.scope;
			}
		} else if (init->opcode != ZEND_INIT_METHOD_CALL) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"direct native method has an unsupported source init");
			return nullptr;
		} else if (init->op1_type == IS_UNUSED) {
			descriptor->receiver_kind =
				ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS;
		} else if (init->op1_type == IS_CV
				|| init->op1_type == IS_VAR
				|| init->op1_type == IS_TMP_VAR) {
			if (!source_descriptor_operand(
					source_op_array, init, init->op1_type,
					init->op1,
					&descriptor->receiver_operand)) {
				std::free(descriptor);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct native method receiver is not explicit");
				return nullptr;
			}
			if (init->op1_type == IS_VAR
					|| init->op1_type == IS_TMP_VAR) {
				descriptor->flags |=
					ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER;
			}
			descriptor->receiver_kind =
				ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT;
			uint64_t receiver_slot =
				descriptor->receiver_operand.index;
			if (descriptor->receiver_operand.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
				receiver_slot += source_op_array->last_var;
			}
			receiver_slot += ZEND_CALL_FRAME_SLOT;
			if (receiver_slot > UINT32_MAX / sizeof(zval)) {
				std::free(descriptor);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct native method receiver frame offset overflows");
				return nullptr;
			}
			descriptor->receiver_source_frame_offset =
				static_cast<uint32_t>(receiver_slot * sizeof(zval));
		} else {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"direct native method receiver is not explicit");
			return nullptr;
		}
	}
	return descriptor;
}

/* One argument of a direct user call: its parameter, mode and exact type,
 * the guard it needs, and whether the frame stays trivial with it. */
static void freeze_direct_call_argument(plan_builder &builder,
	const zend_mir_call_site_ref &site,
	zend_native_direct_call_descriptor *descriptor, zend_function *callee,
	uint32_t n, direct_call_frame &frame) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_call_view *const calls = builder.calls;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	const uint32_t user_binding_count = builder.user_binding_count;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	std::vector<uint8_t> &supplied_parameters = frame.supplied_parameters;
	std::vector<zend_mir_scalar_type_mask> &argument_guard_types =
		frame.argument_guard_types;
	bool &has_argument_guard_type = frame.has_argument_guard_type;
	bool &trivial_frame = frame.trivial_frame;
	const zend_mir_call_argument_ref &argument =
		plan->call_arguments[site.arguments.offset + n];
	/* The direct descriptor already resolved every named
	 * argument to a parameter of this callee. */
	uint32_t parameter_ordinal = argument.ordinal;
	const bool named_parameter_found =
		direct_call_parameter_ordinal(source_op_array,
			callee, argument, &parameter_ordinal);
	ZEND_ASSERT(named_parameter_found);
	(void) named_parameter_found;
	descriptor->arguments[n].ordinal = parameter_ordinal;
	if (parameter_ordinal < supplied_parameters.size()) {
		trivial_frame = trivial_frame
			&& supplied_parameters[parameter_ordinal] == 0;
		supplied_parameters[parameter_ordinal] = 1;
		descriptor->frame_argument_count = std::max(
			descriptor->frame_argument_count,
			parameter_ordinal + 1);
	}
	/*
	 * An open method target is frozen before request-local
	 * binding, so its MIR argument keeps only the SEND
	 * opcode's mode; the bound function's parameter
	 * declaration decides. SEND_VAR_EX to `array &$arg`
	 * transfers a reference cell although it is no SEND_REF.
	 */
	const bool parameter_by_reference =
		ARG_MUST_BE_SENT_BY_REF(
			callee, parameter_ordinal + 1);
	descriptor->arguments[n].mode =
		parameter_by_reference
			|| argument.ownership
				== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
			? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
	const int32_t argument_value_index =
		zend_tpde_value_index(plan, argument.value_id);
	zend_mir_value_id argument_guard_value_id =
		ZEND_MIR_ID_INVALID;
	const int32_t argument_guard_value_index =
		argument_value_index >= 0
		? argument_value_index
		: source_operand_value_id(
			argument.source_operand, argument_guard_value_id)
			? zend_tpde_value_index(
				plan, argument_guard_value_id)
			: -1;
	const zend_mir_storage_id argument_source_storage =
		source_descriptor_storage(
			source_op_array, argument.source_operand);
	descriptor->arguments[n].exact_type =
		descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& argument_value_index >= 0
		? plan->values[argument_value_index].exact_type
		: ZEND_MIR_SCALAR_TYPE_NONE;
	descriptor->arguments[n].source_operand =
		argument.source_operand;
	descriptor->arguments[n].scalar_bits = 0;
	descriptor->arguments[n].source_frame_offset =
		UINT32_MAX;
	if (argument.source_operand.kind
			== ZEND_MIR_SOURCE_OPERAND_SLOT
			|| argument.source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_SSA) {
		uint64_t slot =
			argument.source_operand.index;
		if (argument.source_operand.slot_kind
				!= ZEND_MIR_SOURCE_SLOT_CV) {
			slot += source_op_array->last_var;
		}
		slot += ZEND_CALL_FRAME_SLOT;
		if (slot <= UINT32_MAX / sizeof(zval)) {
			descriptor->arguments[n]
				.source_frame_offset =
					static_cast<uint32_t>(
						slot * sizeof(zval));
		}
	}
	if (argument.source_operand.kind
			== ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& argument.source_operand.index
				< source_op_array->last_literal) {
		/*
		 * The source literal decides: a boxed literal may
		 * share a topology-only MIR value with a NULL
		 * placeholder, which an inline frame must not copy.
		 */
		descriptor->arguments[n].exact_type =
			exact_scalar_from_zval(
				&source_op_array->literals[
					argument.source_operand.index]);
		descriptor->arguments[n].scalar_bits =
			scalar_bits_from_zval(
				&source_op_array->literals[
					argument.source_operand.index]);
	}
	if (!zend_mir_scalar_type_is_exact(
			descriptor->arguments[n].exact_type)
			&& descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
		descriptor->arguments[n].exact_type =
			exact_scalar_from_source_argument(
				source_op_array, source_ssa, calls,
				user_bindings, user_binding_count, argument,
				&builder.exact_scalar_memo);
	}
	/* A parameter CV holds its declared type only while it
	 * still holds the value its RECV received. */
	if (!zend_mir_scalar_type_is_exact(
			descriptor->arguments[n].exact_type)
			&& descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& source_op_array->arg_info != nullptr
			&& (argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& argument.source_operand.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV
			&& argument.source_operand.index
				< source_op_array->num_args
			&& source_argument_is_received_parameter(
				source_op_array, source_ssa, argument)) {
		descriptor->arguments[n].exact_type =
			exact_scalar_from_declared_type(
				source_op_array->arg_info[
					argument.source_operand.index].type);
	}
	const zval *source_literal =
		argument.source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& argument.source_operand.index
				< source_op_array->last_literal
		? &source_op_array->literals[
			argument.source_operand.index]
		: nullptr;
	const bool inline_literal =
		descriptor->arguments[n].mode
			== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
		&& source_literal != nullptr
		&& Z_TYPE_P(source_literal) != IS_CONSTANT_AST;
	const zend_type *parameter_type =
		callee->op_array.arg_info == nullptr
			? nullptr
			: parameter_ordinal < callee->op_array.num_args
				? &callee->op_array.arg_info[
					parameter_ordinal].type
				: (callee->common.fn_flags
						& ZEND_ACC_VARIADIC) != 0
					? &callee->op_array.arg_info[
						callee->op_array.num_args].type
					: nullptr;
	const zend_mir_scalar_type_mask parameter_exact_type =
		parameter_type != nullptr
		? exact_scalar_from_declared_type(*parameter_type)
		: ZEND_MIR_SCALAR_TYPE_NONE;
	const bool guarded_reference =
		descriptor->arguments[n].mode
			== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
		&& zend_mir_scalar_type_is_exact(parameter_exact_type);
	if (guarded_reference) {
		argument_guard_types[n] = parameter_exact_type;
		has_argument_guard_type = true;
	}
	const bool guarded_boxed_value =
		descriptor->arguments[n].mode
			== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
		&& !zend_mir_scalar_type_is_exact(
			descriptor->arguments[n].exact_type)
		&& zend_mir_scalar_type_is_exact(parameter_exact_type)
		&& argument_guard_value_index >= 0
		&& plan->values[argument_guard_value_index].machine_kind
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
		&& zend_mir_id_is_valid(argument_source_storage)
		&& plan->values[argument_guard_value_index]
			.canonical_storage_id == argument_source_storage
		&& descriptor->arguments[n].source_frame_offset
			!= UINT32_MAX;
	if (guarded_boxed_value) {
		argument_guard_types[n] = parameter_exact_type;
		has_argument_guard_type = true;
	}
	const bool inline_argument =
		(descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& (zend_mir_scalar_type_is_exact(
					descriptor->arguments[n].exact_type)
				|| guarded_boxed_value
				|| inline_literal
				|| ((argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& argument.source_operand.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV)
				/* A temporary, or a call result that is
				 * a plain value, moves into the callee
				 * frame; it never holds a reference.
				 * The frame reads it at DO: the direct
				 * descriptor already required that no
				 * opline between SEND and DO names its
				 * slot (the x64 frame copies arguments
				 * before it clears the DO result slot). */
				|| (zend_tpde_linux_inline_forms
					&& (argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& (argument.source_operand.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
						|| (argument.source_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_VAR
							&& call_result_argument_is_value(builder,
								argument)))
					&& descriptor->arguments[n]
						.source_frame_offset
						!= UINT32_MAX)))
		|| (descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			&& (argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& argument.source_operand.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV);
	const bool inline_parameter =
		!trivial_frame
		|| parameter_type == nullptr
		|| !ZEND_TYPE_IS_SET(*parameter_type)
		|| (inline_literal
			&& literal_satisfies_type(
				source_literal, *parameter_type))
		|| (descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& exact_scalar_satisfies_type(
				descriptor->arguments[n].exact_type,
				*parameter_type))
		|| guarded_boxed_value
		|| (descriptor->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			&& guarded_reference);
	trivial_frame =
		trivial_frame && inline_argument && inline_parameter;
}

static bool freeze_direct_call_arguments(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site,
	zend_native_direct_call_descriptor *descriptor, direct_call_frame &frame) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	descriptor->result_operand = site.result_operand;
	descriptor->result_type = ZEND_MIR_SCALAR_TYPE_NONE;
	/* Entry cells always bind a user op array. */
	zend_function *callee =
		plan->instructions[i].entry_cell->function;
	ZEND_ASSERT(callee != nullptr
		&& ZEND_USER_CODE(callee->type));
	std::vector<uint8_t> &supplied_parameters =
		frame.supplied_parameters;
	supplied_parameters.assign(
		callee->common.num_args, 0);
	std::vector<zend_mir_scalar_type_mask>
		&argument_guard_types = frame.argument_guard_types;
	argument_guard_types.assign(
		site.arguments.count, ZEND_MIR_SCALAR_TYPE_NONE);
	bool &has_argument_guard_type = frame.has_argument_guard_type;
	const zend_op_array &op_array = callee->op_array;
	const bool inline_receiver =
		(op_array.scope == nullptr
			&& descriptor->receiver_kind
				== ZEND_NATIVE_INTERNAL_RECEIVER_NONE)
		|| (op_array.scope != nullptr
			&& ((descriptor->receiver_kind
						== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS
					&& (op_array.fn_flags
						& ZEND_ACC_STATIC) == 0)
				|| (descriptor->receiver_kind
						== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE
					&& (op_array.fn_flags
						& ZEND_ACC_STATIC) != 0
					&& (((descriptor->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
							!= 0
						&& descriptor->called_scope == nullptr)
						|| ((descriptor->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
							== 0
						&& descriptor->called_scope != nullptr)))
				|| (descriptor->receiver_kind
						== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
					&& (op_array.fn_flags
						& ZEND_ACC_STATIC) == 0
					&& (descriptor->receiver_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| descriptor->receiver_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& descriptor->receiver_source_frame_offset
						!= UINT32_MAX)));
	bool &trivial_frame = frame.trivial_frame;
	trivial_frame =
		inline_receiver
		&& (op_array.scope == nullptr
			|| (op_array.scope->ce_flags & ZEND_ACC_TRAIT) == 0)
		&& site.arguments.count
			>= op_array.required_num_args
		&& (op_array.fn_flags
			& (ZEND_ACC_CALL_VIA_TRAMPOLINE
				| ZEND_ACC_DEPRECATED
				| ZEND_ACC_NODISCARD)) == 0;
	if (zend_mir_id_is_valid(record.result_id)) {
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"direct user-call result has no value record");
			return false;
		}
		if (zend_mir_scalar_type_is_exact(
				plan->values[result_index].exact_type)) {
			descriptor->result_type =
				plan->values[result_index].exact_type;
		}
	}
	if (!zend_mir_scalar_type_is_exact(
			descriptor->result_type)
			&& callee->op_array.arg_info != nullptr
			&& (callee->common.fn_flags
					& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		descriptor->result_type =
			exact_scalar_from_declared_type(
				callee->op_array.arg_info[-1].type);
	}
	for (uint32_t n = 0; n < site.arguments.count; ++n) {
		freeze_direct_call_argument(
			builder, site, descriptor, callee, n, frame);
	}
	if (trivial_frame) {
		const zend_op_array &op_array = callee->op_array;
		for (uint32_t parameter = 0;
				trivial_frame && parameter < op_array.num_args;
				++parameter) {
			if (supplied_parameters[parameter] != 0) {
				continue;
			}
			const zend_op &receive = op_array.opcodes[parameter];
			const zval *default_value =
				receive.opcode == ZEND_RECV_INIT
					&& receive.op1.num == parameter + 1
					&& receive.op2_type == IS_CONST
					&& EX_VAR_TO_NUM(receive.result.var) == parameter
				? RT_CONSTANT(&receive, receive.op2)
				: nullptr;
			trivial_frame = default_value != nullptr
				&& default_value >= op_array.literals
				&& default_value
					< op_array.literals + op_array.last_literal
				&& Z_TYPE_P(default_value) != IS_CONSTANT_AST;
			if (trivial_frame) {
				zend_native_direct_call_default_literals(
					descriptor)[parameter] =
						static_cast<uint32_t>(
							default_value - op_array.literals);
			}
		}
		if (trivial_frame) {
			descriptor->frame_size = zend_vm_calc_used_stack(
				descriptor->frame_argument_count, callee);
		}
	}
	return true;
}

static bool freeze_direct_call_inline_frame(
	const plan_builder &builder, uint32_t i, const zend_mir_call_site_ref &site,
	int32_t binding_index, zend_native_direct_call_descriptor *descriptor,
	const direct_call_frame &frame) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	zend_native_diagnostic *const diag = builder.diag;
	zend_function *callee = plan->instructions[i].entry_cell->function;
	const std::vector<zend_mir_scalar_type_mask> &argument_guard_types =
		frame.argument_guard_types;
	const bool has_argument_guard_type = frame.has_argument_guard_type;
	const bool trivial_frame = frame.trivial_frame;
	const bool inline_result =
		descriptor->result_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED
		|| ((descriptor->result_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| descriptor->result_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (descriptor->result_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_CV
				|| descriptor->result_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
				|| descriptor->result_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_VAR));
	if (trivial_frame && inline_result) {
		for (uint32_t n = 0; n < site.arguments.count; ++n) {
			if (descriptor->arguments[n].mode
					== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
					&& zend_mir_scalar_type_is_exact(
						argument_guard_types[n])) {
				descriptor->arguments[n].exact_type =
					argument_guard_types[n];
			}
		}
		if (has_argument_guard_type) {
			auto *guard_types =
				static_cast<zend_mir_scalar_type_mask *>(
					std::malloc(
						static_cast<size_t>(site.arguments.count)
							* sizeof(zend_mir_scalar_type_mask)));
			if (guard_types == nullptr) {
				std::free(descriptor);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
					"unable to allocate inline argument guards");
				return false;
			}
			std::memcpy(guard_types, argument_guard_types.data(),
				static_cast<size_t>(site.arguments.count)
					* sizeof(*guard_types));
			plan->instructions[i]
				.direct_call_argument_guard_types = guard_types;
		}
		descriptor->flags |=
			ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME;
		if ((callee->op_array.fn_flags & ZEND_ACC_VARIADIC) != 0) {
			require_runtime_helper(
				plan,
				ZEND_NATIVE_HELPER_RECEIVE_EXPLICIT_PENDING);
		}
		bool leaf_scalar_frame =
			user_bindings[binding_index].leaf_scalar_frame
			&& (callee->op_array.fn_flags & ZEND_ACC_VARIADIC) == 0
			&& descriptor->receiver_kind
				== ZEND_NATIVE_INTERNAL_RECEIVER_NONE
			&& site.arguments.count
				== callee->op_array.num_args
			&& (descriptor->result_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| zend_mir_scalar_type_is_exact(
					descriptor->result_type));
		/* The leaf frame copies payloads as their exact type
		 * without checking it: an argument whose type only a
		 * run-time guard proves takes the guarded frame. */
		for (uint32_t n = 0;
				leaf_scalar_frame
					&& n < site.arguments.count; ++n) {
			leaf_scalar_frame =
				descriptor->arguments[n].ordinal == n
				&& descriptor->arguments[n].mode
					== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
				&& zend_mir_scalar_type_is_exact(
					descriptor->arguments[n].exact_type)
				&& !zend_mir_scalar_type_is_exact(
					argument_guard_types[n]);
		}
		if (leaf_scalar_frame) {
			descriptor->flags |=
				ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME;
		}
	}
	return true;
}

static bool freeze_direct_user_call(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site, const zend_mir_call_target_ref &target,
	int32_t binding_index, zend_function *expected_function,
	const zend_op_array *expected_op_array) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_direct_call_descriptor *descriptor =
		create_direct_user_call_descriptor(builder, i, site, target,
			expected_function, expected_op_array);
	if (descriptor == nullptr) {
		return false;
	}
	direct_call_frame frame;
	if (!freeze_direct_call_arguments(builder, i, site, descriptor, frame)
			|| !freeze_direct_call_inline_frame(
				builder, i, site, binding_index, descriptor, frame)) {
		return false;
	}
	zend_native_direct_call_descriptor_derive(descriptor);
	plan->instructions[i].direct_call = descriptor;
	plan->direct_calls[plan->direct_call_count++] = descriptor;
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE);
	return true;
}

static bool freeze_source_user_call(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site, const zend_mir_call_target_ref &target,
	bool fragment_call) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	if (site.source_init_opline_index
			>= source_op_array->last) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"user call has no explicit source init descriptor");
		return false;
	}
	const zend_op *init = &source_op_array->opcodes[
		site.source_init_opline_index];
	plan->instructions[i].user_call_no_call =
		init->opcode == ZEND_NEW
		&& target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER
		&& target.num_args == 0
		&& target.required_num_args == 0
		&& plan->instructions[i].entry_cell != nullptr
		&& plan->instructions[i].entry_cell->function
			== reinterpret_cast<const zend_function *>(
				source_op_array);
	auto *descriptor = build_user_call_descriptor(
		plan, source_op_array, site, record, diag);
	if (descriptor == nullptr) {
		return false;
	}
	plan->instructions[i].user_call = descriptor;
	plan->user_calls[plan->user_call_count++] = descriptor;
	if (fragment_call) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_CALL_FRAGMENT);
	}
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
			|| descriptor->do_opcode
				== ZEND_CALLABLE_CONVERT_PARTIAL) {
		require_runtime_helper(
				plan,
				ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT);
	} else {
		/* Everything a source-backed user call may reach. */
		static constexpr zend_native_runtime_helper_id
			user_call_helpers[] = {
				ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT,
				ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER,
				ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE,
				ZEND_NATIVE_HELPER_USER_CALL_RESOLVE,
				ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION,
				ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION,
				ZEND_NATIVE_HELPER_CALL_RESERVE_DYNAMIC_FRAME,
				ZEND_NATIVE_HELPER_USER_CALL_EXPAND_ARGUMENTS,
				ZEND_NATIVE_HELPER_USER_CALL_SEND_RESOLVED_ARGUMENT,
				ZEND_NATIVE_HELPER_FRAME_PREPARE,
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_BEGIN,
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_END,
				ZEND_NATIVE_HELPER_FRAME_FINALIZE,
				ZEND_NATIVE_HELPER_CALL_FAST_LEAVE,
				ZEND_NATIVE_HELPER_CALL_FAST_UNDEFINED_ARGUMENT,
				ZEND_NATIVE_HELPER_CALL_FAST_SCALAR_VIOLATION,
				ZEND_NATIVE_HELPER_CALL_FAST_DYNAMIC_INIT,
				ZEND_NATIVE_HELPER_CALL_FAST_METHOD_INIT,
				ZEND_NATIVE_HELPER_CALL_FAST_SEND,
				ZEND_NATIVE_HELPER_CALL_UNIVERSAL_INIT,
				ZEND_NATIVE_HELPER_CALL_UNIVERSAL_DO,
				ZEND_NATIVE_HELPER_CALL_UNIVERSAL_SEND,
				ZEND_NATIVE_HELPER_CALL_FAST_NEW,
				ZEND_NATIVE_HELPER_CALL_FAST_SEND_VAR_REFERENCE,
				ZEND_NATIVE_HELPER_CALL_FAST_DO,
				ZEND_NATIVE_HELPER_CALL_FAST_REARM,
				ZEND_NATIVE_HELPER_CALL_UNIVERSAL_EXPAND,
				ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RESERVE,
				ZEND_NATIVE_HELPER_CHECK_FUNC_ARG_RESOLVED,
				ZEND_NATIVE_HELPER_FRAME_ACTIVATION_POP,
				ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE,
				ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION,
			};
		for (const zend_native_runtime_helper_id helper
				: user_call_helpers) {
			require_runtime_helper(plan, helper);
		}
	}
	return true;
}

static bool plan_direct_user_call(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site, const zend_mir_call_target_ref &target,
	bool fragment_call) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	zend_native_diagnostic *const diag = builder.diag;
	const uint32_t count = plan->instructions[i].operand_count;
	const bool source_arguments = count == 0
		&& site.arguments.count != 0;
	const int32_t binding_index = id_index_find(
		plan->user_binding_index,
		plan->user_binding_index_capacity, site.target_id);
	if (binding_index < 0) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
			"direct user call has no native entry-cell binding");
		return false;
	}
	plan->instructions[i].entry_cell =
		user_bindings[binding_index].entry_cell;
	plan->instructions[i].component_target_index =
		user_bindings[binding_index].component_target_index;
	freeze_numeric_variant_target(builder, i, site, binding_index);
	zend_function *expected_function =
		plan->instructions[i].entry_cell != nullptr
			? plan->instructions[i].entry_cell->function
			: nullptr;
	const zend_op_array *expected_op_array =
		expected_function != nullptr
				&& ZEND_USER_CODE(expected_function->type)
			? &expected_function->op_array : nullptr;
	const bool direct_descriptor = direct_user_call_descriptor_eligible(
		builder, site, target, binding_index, fragment_call, expected_function);
	if (direct_descriptor) {
		if (!freeze_direct_user_call(builder, i, site, target, binding_index,
				expected_function, expected_op_array)) {
			return false;
		}
	} else if (!freeze_source_user_call(
			builder, i, site, target, fragment_call)) {
		return false;
	}
	if (!fragment_call
			&& source_arguments && !direct_descriptor) {
		for (uint32_t n = 0; n < site.arguments.count; ++n) {
			const zend_mir_call_argument_ref &argument =
				plan->call_arguments[site.arguments.offset + n];
			if (argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-backed user call has a scalar argument");
				return false;
			}
		}
	}
	return true;
}

static bool freeze_direct_internal_call(
	plan_builder &builder, uint32_t i, const zend_mir_call_site_ref &site) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const zend_op *init = &source_op_array->opcodes[
	site.source_init_opline_index];
	const zend_op *finish = &source_op_array->opcodes[
	site.source_do_opline_index];
	if ((finish->opcode != ZEND_DO_ICALL
			&& finish->opcode != ZEND_DO_FCALL
			&& finish->opcode != ZEND_DO_FCALL_BY_NAME
			&& finish->opcode != ZEND_CALLABLE_CONVERT
			&& finish->opcode
				!= ZEND_CALLABLE_CONVERT_PARTIAL)
		|| init->extended_value > site.arguments.count) {
		zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"direct internal call source sequence is inconsistent");
		return false;
	}
	const size_t descriptor_size = offsetof(
		zend_native_direct_internal_call_descriptor, arguments)
	+ static_cast<size_t>(site.arguments.count)
		* sizeof(zend_native_direct_internal_call_argument);
	auto *descriptor =
	static_cast<zend_native_direct_internal_call_descriptor *>(
		std::calloc(1, descriptor_size));
	if (descriptor == nullptr) {
		zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
		"unable to allocate a direct internal-call descriptor");
		return false;
	}
	descriptor->argument_count = site.arguments.count;
	descriptor->initial_argument_count = init->extended_value;
	descriptor->init_source_position =
		site.source_init_opline_index;
	descriptor->do_source_position = site.source_do_opline_index;
	descriptor->do_opcode = finish->opcode;
	descriptor->do_op1_payload = finish->op1.num;
	descriptor->do_extended_value = finish->extended_value;
	descriptor->result_operand = site.result_operand;
	descriptor->result_type = ZEND_MIR_SCALAR_TYPE_NONE;
	if (finish->opcode != ZEND_CALLABLE_CONVERT
			&& finish->opcode
				!= ZEND_CALLABLE_CONVERT_PARTIAL
			&& (site.result_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| ((site.result_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| site.result_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& site.result_operand.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP))) {
		descriptor->flags |=
			ZEND_NATIVE_DIRECT_INTERNAL_CALL_FAST_FINISH;
	}
	zend_function *callee =
		plan->instructions[i].internal_call_cell != nullptr
			? plan->instructions[i].internal_call_cell->function
			: nullptr;
	if (!source_descriptor_operand(
		source_op_array, init, init->op1_type, init->op1,
		&descriptor->receiver_operand)
			|| !source_descriptor_operand(
				source_op_array, finish, finish->op2_type,
				finish->op2, &descriptor->do_op2)) {
		std::free(descriptor);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct internal-call source operand is invalid");
		return false;
	}
	if (zend_mir_id_is_valid(record.result_id)) {
		const int32_t result_index =
		zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0
			|| !zend_mir_scalar_type_is_exact(
				plan->values[result_index].exact_type)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct internal-call result has no exact scalar value");
			return false;
		}
		descriptor->result_type =
			plan->values[result_index].exact_type;
	}
	if (!zend_mir_scalar_type_is_exact(
			descriptor->result_type)) {
		if (callee != nullptr && callee->common.arg_info != nullptr
				&& (callee->common.fn_flags
					& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
			descriptor->result_type =
				exact_scalar_from_declared_type(
					callee->common.arg_info[-1].type);
		}
	}
	for (uint32_t n = 0; n < site.arguments.count; ++n) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
				plan, site.arguments.offset + n, &argument)
			|| argument.send_opline_index
				>= source_op_array->last) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct internal-call argument table is unreadable");
			return false;
		}
		const zend_op *send = &source_op_array->opcodes[
		argument.send_opline_index];
		if (!source_descriptor_send_opcode(send->opcode)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct internal-call SEND opcode is invalid");
			return false;
		}
		zend_native_direct_internal_call_argument &encoded =
		descriptor->arguments[n];
		encoded.ordinal = argument.ordinal;
		bool send_by_reference = argument.ownership
			== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
		if (argument.source_mode
				== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
			uint32_t parameter_ordinal;
			send_by_reference =
				direct_call_parameter_ordinal(
					source_op_array, callee, argument,
					&parameter_ordinal)
				&& ARG_MUST_BE_SENT_BY_REF(
					callee, parameter_ordinal + 1);
		}
		encoded.mode = argument.source_mode
			== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
		? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
		: send_by_reference
			? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
		encoded.source_opcode = send->opcode;
		encoded.source_position = argument.send_opline_index;
		encoded.source_operand = argument.source_operand;
		encoded.auxiliary_payload = send->op2.num;
		encoded.result_payload = send->result.num;
		encoded.extended_value = send->extended_value;
		if (!source_descriptor_operand(
			source_op_array, send, send->op2_type, send->op2,
			&encoded.auxiliary_operand)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct internal-call auxiliary operand is invalid");
			return false;
		}
	}
	plan->instructions[i].direct_internal_call = descriptor;
	plan->direct_internal_calls[
		plan->direct_internal_call_count++] = descriptor;
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_PLAIN);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_PUSH);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN);
	require_runtime_helper(plan,
		ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN_FINISH);
	require_runtime_helper(plan,
		ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT);
	require_runtime_helper(plan,
		ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE);
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR);
	require_runtime_helper(plan,
		ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT);
	return true;
}

static bool plan_direct_internal_call(plan_builder &builder, uint32_t i,
	const zend_mir_call_site_ref &site, bool fragment_call) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_internal_call_binding *const internal_bindings =
		builder.internal_bindings;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const int32_t binding_index = id_index_find(
		plan->internal_binding_index,
		plan->internal_binding_index_capacity, site.target_id);
	if (binding_index < 0
			|| site.source_init_opline_index
				>= source_op_array->last
			|| site.source_do_opline_index
				>= source_op_array->last) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
			"direct internal call has no runtime binding or source descriptor");
		return false;
	}
	plan->instructions[i].internal_call_cell =
		internal_bindings[binding_index].call_cell;
	if (fragment_call) {
		zend_native_user_call_descriptor *descriptor =
			build_user_call_descriptor(
				plan, source_op_array, site, record, diag);
		if (descriptor == nullptr) {
			return false;
		}
		plan->instructions[i].user_call = descriptor;
		plan->user_calls[
			plan->user_call_count++] = descriptor;
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_CALL_FRAGMENT);
	} else if (!freeze_direct_internal_call(builder, i, site)) {
		return false;
	}
	return true;
}

/*
 * A direct user or internal call: its call site, whether it keeps its
 * source INIT/SEND/DO fragments, and its descriptor.
 */
bool plan_call_site(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_call_view *const calls = builder.calls;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	zend_mir_call_site_ref site{};
	zend_mir_call_target_ref target{};
	zend_mir_call_continuation_ref exception_continuation{};
	const int32_t site_index = id_index_find(
		plan->call_site_instruction_index,
		plan->call_site_instruction_index_capacity, record.id);
	if (calls == nullptr || site_index < 0
			|| !calls->call_site_at(
				calls->context,
				static_cast<uint32_t>(site_index), &site)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct call lacks its call view");
		return false;
	}
	const int32_t target_index = id_index_find(
		plan->call_target_index, plan->call_target_index_capacity,
		site.target_id);
	if (site.instruction_id != record.id || target_index < 0
			|| site.arguments.offset > plan->call_argument_count
			|| site.arguments.count > plan->call_argument_count
				- site.arguments.offset
			|| !calls->call_target_at(
				calls->context,
				static_cast<uint32_t>(target_index), &target)
			|| target.id != site.target_id
			|| site.continuations.count != 4
			|| !calls->call_continuation_at(calls->context,
				site.continuations.offset + 1,
				&exception_continuation)
			|| exception_continuation.kind
				!= ZEND_MIR_CALL_CONTINUATION_EXCEPTION
			|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& ((target.kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
						&& target.kind
							!= ZEND_MIR_CALL_TARGET_METHOD_USER
						&& target.kind
							!= ZEND_MIR_CALL_TARGET_DYNAMIC)
					|| (site.arguments.count != count
						&& count != 0)))
			|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
				&& (count != 0
					|| target.kind
						!= ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL))) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct call instruction and call site disagree");
		return false;
	}
	plan->instructions[i].call_site =
		&plan->call_sites[static_cast<uint32_t>(site_index)];
	plan->instructions[i].exception_block_id =
		exception_continuation.block_id;
	plan->instructions[i].call_argument_offset = site.arguments.offset;
	plan->instructions[i].call_argument_count = site.arguments.count;
	if (source_op_array == nullptr
			|| site.source_do_opline_index >= source_op_array->last) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"direct call has no explicit source completion descriptor");
		return false;
	}
	/* NEW creates its receiver in the INIT opcode, but a direct
	 * internal descriptor only carries an existing receiver: inherited
	 * internal constructors keep the source phases. */
	const bool internal_constructor_call =
		record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
		&& site.source_init_opline_index < source_op_array->last
		&& source_op_array->opcodes[
			site.source_init_opline_index].opcode == ZEND_NEW;
	/* The binding cell records the declaring scope, but
	 * INIT_STATIC_METHOD_CALL may name a derived called scope: keep
	 * the source INIT for late static binding. */
	const bool internal_static_method_call =
		record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
		&& site.source_init_opline_index < source_op_array->last
		&& source_op_array->opcodes[
			site.source_init_opline_index].opcode
			== ZEND_INIT_STATIC_METHOD_CALL;
	/* An internal handler may validate before its parameter parser
	 * reports too few arguments, so an under-arity call keeps its
	 * INIT/SEND/DO and reaches the handler with the frame state of
	 * ZEND_DO_ICALL. */
	const bool internal_under_arity_call =
		record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
		&& site.arguments.count < target.required_num_args;
	bool nested_source_call = participates_in_nested_call(builder,
		static_cast<uint32_t>(site_index));
	if (nested_source_call && zend_tpde_linux_inline_forms) {
		nested_source_call = !nested_call_group_direct(builder,
			static_cast<uint32_t>(site_index));
	}
	const bool fragment_call =
		internal_constructor_call
		|| internal_static_method_call
		|| internal_under_arity_call
		|| nested_source_call
		|| call_site_requires_source_fragments(plan, site);
	plan->instructions[i].user_opcode_call_fragments =
		fragment_call;
	if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		if (!plan_direct_user_call(builder, i, site, target, fragment_call)) {
			return false;
		}
	} else if (!plan_direct_internal_call(builder, i, site, fragment_call)) {
		return false;
	}
	if (zend_mir_id_is_valid(record.result_id)
			&& plan->instructions[i].direct_call == nullptr
			&& plan->instructions[i].direct_internal_call == nullptr
			&& (plan->instructions[i].user_call == nullptr
				|| plan->instructions[i].user_call->do_opcode
					== ZEND_CALLABLE_CONVERT
				|| plan->instructions[i].user_call->do_opcode
					== ZEND_CALLABLE_CONVERT_PARTIAL)) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR);
	}
	return true;
}

} // namespace zend::native::tpde::plan_detail
