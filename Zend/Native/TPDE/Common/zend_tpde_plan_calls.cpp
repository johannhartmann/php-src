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

} // namespace zend::native::tpde::plan_detail
