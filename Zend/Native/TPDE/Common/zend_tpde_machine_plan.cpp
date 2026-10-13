// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

#include <span>

namespace zend::native::tpde::plan_detail {

static zend_tpde_local_abi_type machine_plan_abi(
		zend_mir_representation representation,
		zend_mir_scalar_type_mask exact_type,
		zend_tpde_machine_value_kind machine_kind,
		zend_tpde_local_abi_transfer transfer) {
	zend_tpde_local_abi_type result{};
	result.representation = representation;
	result.exact_type = exact_type;
	result.machine_kind = machine_kind;
	result.transfer = transfer;
	result.valid = true;
	return result;
}

static bool machine_plan_abi_same_shape(
		const zend_tpde_local_abi_type &left,
		const zend_tpde_local_abi_type &right) {
	return left.valid && right.valid
		&& left.representation == right.representation
		&& left.exact_type == right.exact_type
		&& left.machine_kind == right.machine_kind;
}

static bool machine_plan_abi_can_supply_argument(
		const zend_tpde_local_abi_type &caller,
		const zend_tpde_local_abi_type &callee) {
	/* A scalar is boxed for a zval parameter; it owns nothing. */
	if (caller.valid && callee.valid
			&& callee.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& callee.representation == ZEND_MIR_REPRESENTATION_ZVAL
			&& caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE
			&& ((caller.exact_type == ZEND_MIR_SCALAR_TYPE_I64
					&& caller.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64)
				|| (caller.exact_type == ZEND_MIR_SCALAR_TYPE_F64
					&& caller.machine_kind == ZEND_TPDE_MACHINE_VALUE_F64)
				|| (caller.exact_type == ZEND_MIR_SCALAR_TYPE_I1
					&& caller.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOOL))) {
		return true;
	}
	if (!machine_plan_abi_same_shape(caller, callee)) {
		return false;
	}
	switch (callee.transfer) {
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED:
			/* A typed body borrows its argument for the duration of the call.
			 * An existing owner or immortal root is therefore a valid source;
			 * no ownership operation is required at the call boundary. */
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED
				|| caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED
				|| caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED:
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
	}
	return false;
}

static bool machine_plan_call_argument_can_supply(
		const zend_tpde_plan *plan,
		const zend_mir_call_argument_ref &argument,
		const zend_tpde_local_abi_type &caller,
		const zend_tpde_local_abi_type &callee) {
	if (!machine_plan_abi_can_supply_argument(caller, callee)) {
		return false;
	}
	if (callee.transfer != ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED
			|| caller.transfer != ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED) {
		return true;
	}
	if (plan->source_opcodes == nullptr
			|| argument.send_opline_index >= plan->source_opcode_count) {
		return false;
	}
	const uint8_t source_type =
		plan->source_opcodes[argument.send_opline_index].op1_type;
	return source_type == IS_CV || source_type == IS_CONST;
}

static zend_tpde_local_abi_type machine_plan_value_abi(
		const zend_tpde_plan *plan, uint32_t value_index) {
	if (value_index >= plan->value_count) {
		return {};
	}
	for (uint32_t depth = 0; depth < plan->value_count; ++depth) {
		const int32_t alias =
			plan->values[value_index].register_alias_value_index;
		if (alias < 0) {
			break;
		}
		if (static_cast<uint32_t>(alias) >= plan->value_count) {
			return {};
		}
		if (static_cast<uint32_t>(alias) == value_index) {
			break;
		}
		value_index = static_cast<uint32_t>(alias);
	}
	const zend_tpde_value &value = plan->values[value_index];
	if (value.local_abi.valid) {
		return value.local_abi;
	}
	if (value.argument_index >= 0
			&& plan->argument_abi != nullptr
			&& static_cast<uint32_t>(value.argument_index)
				< plan->argument_count) {
		return plan->argument_abi[
			static_cast<uint32_t>(value.argument_index)];
	}
	const bool exact_scalar =
		zend_mir_scalar_type_is_exact(value.exact_type)
		&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL;
	const bool native_pointer =
		zend_tpde_machine_kind_is_pointer(value.machine_kind);
	const bool boxed =
		value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	if ((!exact_scalar && !native_pointer && !boxed)
			|| (!exact_scalar && value.canonical_alias_observable)) {
		return {};
	}
	const zend_tpde_machine_value_kind abi_kind =
		exact_scalar
			? zend_tpde_scalar_machine_kind(value.exact_type)
			: value.machine_kind;
	const zend_tpde_machine_representation_desc machine_rep =
		zend_tpde_machine_representation(abi_kind, true);
	if (machine_rep.part_count == 0 || machine_rep.parts == nullptr) {
		return {};
	}
	return machine_plan_abi(
		exact_scalar
			? zend_tpde_scalar_representation(value.exact_type)
		: native_pointer
			? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			: value.representation,
		value.exact_type,
		abi_kind,
		exact_scalar
			? ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE
			: value.refcount_state == ZEND_MIR_REFCOUNT_IMMORTAL
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL
			: value.ownership == ZEND_MIR_OWNERSHIP_STATE_MOVED
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED
			: value.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED
					|| value.ownership
						== ZEND_MIR_OWNERSHIP_STATE_SHARED_OWNED
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED
				: ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED);
}

static int32_t machine_plan_source_value_index(
		const zend_tpde_plan *plan,
		const zend_mir_source_operand_ref &operand) {
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_id = zend_mir_value_from_synthetic(operand.index);
	} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.ssa_variable_id != ZEND_MIR_ID_INVALID) {
		value_id =
			zend_mir_value_from_original_ssa(operand.ssa_variable_id);
	}
	return zend_mir_id_is_valid(value_id)
		? zend_tpde_value_index(plan, value_id) : -1;
}

bool machine_plan_type_check_supported(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| instruction.value_operation.source_opcode
				!= ZEND_TYPE_CHECK
			|| instruction.value_operation.result.ssa_variable_id
				== ZEND_MIR_ID_INVALID) {
		return false;
	}
	const int32_t input_index = machine_plan_source_value_index(
		plan, instruction.value_operation.op1);
	const int32_t result_index = machine_plan_source_value_index(
		plan, instruction.value_operation.result);
	if (input_index < 0
			|| (result_index >= 0
				&& static_cast<uint32_t>(result_index)
					>= plan->value_count)) {
		return false;
	}
	switch (plan->values[input_index].exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
		case ZEND_MIR_SCALAR_TYPE_I1:
		case ZEND_MIR_SCALAR_TYPE_I64:
		case ZEND_MIR_SCALAR_TYPE_F64:
			return true;
		default:
			return false;
	}
}

/* Per-call scratch of try_freeze_typed_body_signature, reused across members
 * and fixpoint passes. */
struct typed_body_signature_scratch {
	std::vector<zend_tpde_local_abi_type> call_result_types;
	std::vector<zend_tpde_local_abi_type> instruction_result_types;
	std::vector<zend_tpde_local_abi_type> register_source_ssa;
};

/* A variadic receive constructs a packed array from a dynamic number of
 * arguments. The fixed typed-body ABI does not carry enough information to
 * reproduce that operation. */
static bool plan_receives_variadic(const zend_tpde_plan *plan) {
	if (plan->source_opcodes == nullptr) {
		return false;
	}
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		if (plan->source_opcodes[source].opcode == ZEND_RECV_VARIADIC) {
			return true;
		}
	}
	return false;
}

/* The caller rejects members that receive variadic arguments
 * (plan_receives_variadic). */
[[nodiscard]]
static bool try_freeze_typed_body_signature(
		const zend_tpde_plan *plan,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		const uint8_t *typed_body_candidates,
		typed_body_signature_scratch &scratch,
		zend_tpde_local_abi_type *return_type,
		bool *may_fail) {
	*may_fail = false;
	if (component_plans == nullptr
			|| typed_body_candidates == nullptr || return_type == nullptr
			|| plan->generator_resume_count != 0
			|| plan->user_opcode_callbacks
			|| (plan->argument_count != 0
				&& plan->argument_value_indices == nullptr)) {
		return false;
	}
	for (uint32_t argument = 0;
			argument < plan->argument_count; ++argument) {
		const int32_t value_index =
			plan->argument_value_indices[argument];
		if (value_index < 0
				|| static_cast<uint32_t>(value_index)
					>= plan->value_count
				|| !machine_plan_value_abi(
					plan, static_cast<uint32_t>(value_index)).valid) {
			return false;
		}
	}

	zend_tpde_local_abi_type result_type{};
	std::vector<zend_tpde_local_abi_type> &call_result_types =
		scratch.call_result_types;
	std::vector<zend_tpde_local_abi_type> &instruction_result_types =
		scratch.instruction_result_types;
	std::vector<zend_tpde_local_abi_type> &register_source_ssa =
		scratch.register_source_ssa;
	call_result_types.assign(plan->value_count, {});
	instruction_result_types.assign(plan->instruction_count, {});
	register_source_ssa.assign(plan->source_ssa_variable_count, {});
	auto value_has_typed_body_definition = [&](int32_t value_index) {
		for (uint32_t depth = 0;
				value_index >= 0 && depth < plan->value_count;
				++depth) {
			if (static_cast<uint32_t>(value_index) >= plan->value_count) {
				return false;
			}
			const zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(value_index)];
			if (value.constant || value.argument_index >= 0
					|| (plan->value_definition_instructions != nullptr
						&& plan->value_definition_instructions[
							static_cast<uint32_t>(value_index)] >= 0)) {
				return true;
			}
			/* A typed numeric operation or forwarding assignment defines
			 * its result in registers. */
			if (plan->source_value_definition_instructions != nullptr) {
				const int32_t producer =
					plan->source_value_definition_instructions[
						static_cast<uint32_t>(value_index)];
				if (producer >= 0
						&& static_cast<uint32_t>(producer)
							< plan->instruction_count
						&& (zend_tpde_typed_numeric_binary(
								plan, plan->instructions[producer])
							|| zend_tpde_typed_forward_assign(
								plan->instructions[producer]))) {
					return true;
				}
			}
			if (value.register_alias_value_index < 0
					|| value.register_alias_value_index == value_index) {
				return false;
			}
			value_index = value.register_alias_value_index;
		}
		return false;
	};
	bool saw_return = false;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.local_abi_transport) {
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const uint32_t target = instruction.component_target_index;
			const zend_tpde_plan *callee =
				target < component_count ? component_plans[target] : nullptr;
			if (instruction.direct_call == nullptr
					|| target >= component_count
					|| typed_body_candidates[target] == 0
					|| instruction.direct_call->receiver_kind
						!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
					|| callee == nullptr
					|| (callee->argument_count != 0
						&& callee->argument_value_indices == nullptr)
					|| instruction.call_argument_count
						!= callee->argument_count) {
				return false;
			}
			for (uint32_t argument = 0;
					argument < instruction.call_argument_count;
					++argument) {
				zend_mir_call_argument_ref source_argument{};
				const int32_t callee_value =
					callee->argument_value_indices[argument];
				const uint32_t argument_index =
					instruction.call_argument_offset + argument;
				if (!zend_tpde_call_argument_at(
						plan, argument_index, &source_argument)
						|| (source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
							&& source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
							&& source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE)
						|| callee_value < 0
						|| static_cast<uint32_t>(callee_value)
							>= callee->value_count) {
					return false;
				}
				const zend_tpde_source_value_binding caller_binding =
					plan->call_argument_bindings[argument_index];
				zend_tpde_local_abi_type caller_abi{};
				if (caller_binding.value_index >= 0
						&& static_cast<uint32_t>(
							caller_binding.value_index)
							< plan->value_count) {
					caller_abi = machine_plan_value_abi(
						plan, static_cast<uint32_t>(
							caller_binding.value_index));
				}
				if (!caller_abi.valid
						&& caller_binding.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							caller_binding.definition_instruction_index)
							< instruction_result_types.size()) {
					caller_abi = instruction_result_types[
						static_cast<uint32_t>(
							caller_binding.definition_instruction_index)];
				}
				const zend_mir_value_id caller_ssa =
					source_argument.source_operand.ssa_variable_id;
				if (!caller_abi.valid
						&& caller_ssa < register_source_ssa.size()) {
					caller_abi = register_source_ssa[caller_ssa];
				}
				const zend_tpde_local_abi_type callee_abi =
					machine_plan_value_abi(
						callee, static_cast<uint32_t>(callee_value));
				const bool by_reference =
					source_argument.ownership
						== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
				if (!machine_plan_abi_can_supply_argument(
						caller_abi, callee_abi)
						|| by_reference
							!= (callee_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
					return false;
				}
			}
			const int32_t call_result = machine_plan_source_value_index(
				plan, instruction.direct_call->result_operand);
			const zend_mir_value_id call_result_ssa =
				instruction.direct_call->result_operand.ssa_variable_id;
			zend_tpde_local_abi_type call_result_type =
				callee->return_abi;
			if (!call_result_type.valid && call_result >= 0) {
				call_result_type = machine_plan_value_abi(
					plan, static_cast<uint32_t>(call_result));
			} else if (!call_result_type.valid
					&& zend_mir_scalar_type_is_exact(
						instruction.direct_call->result_type)
					&& instruction.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_NULL) {
				call_result_type = zend_tpde_scalar_local_abi(
					instruction.direct_call->result_type);
			}
			if (!call_result_type.valid) {
				return false;
			}
			instruction_result_types[index] = call_result_type;
			if (call_result >= 0) {
				call_result_types[static_cast<uint32_t>(call_result)] =
					call_result_type;
			} else if (call_result_ssa < register_source_ssa.size()) {
				register_source_ssa[call_result_ssa] = call_result_type;
			} else if (instruction.source_result_binding
							.definition_instruction_index
						!= static_cast<int32_t>(index)) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
			zend_tpde_local_abi_type verified_type{};
			const int32_t verified =
				instruction.source_op1_binding.value_index;
			if (instruction.source_op1_binding
						.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)
						< instruction_result_types.size()) {
				verified_type = instruction_result_types[
					static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)];
			}
			const zend_mir_value_id verified_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			if (!verified_type.valid
					&& verified_ssa < register_source_ssa.size()) {
				verified_type = register_source_ssa[verified_ssa];
			}
			if (!verified_type.valid && verified >= 0
					&& static_cast<uint32_t>(verified)
						< plan->value_count) {
				verified_type = machine_plan_value_abi(
					plan, static_cast<uint32_t>(verified));
			}
			/* A number returned as int or float is checked at the return. */
			if (zend_tpde_machine_kind_is_number(plan->return_abi.machine_kind)
					&& zend_tpde_binding_is_number(plan, component_plans,
						component_count, instruction.source_op1_binding)) {
				continue;
			}
			/* A boxed zval proves no declared type; keep the check. */
			if (!machine_plan_abi_same_shape(
					verified_type, plan->return_abi)
					|| plan->return_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK) {
			if (!machine_plan_type_check_supported(plan, instruction)) {
				return false;
			}
			const zend_mir_value_id result_ssa =
				instruction.value_operation.result.ssa_variable_id;
			if (result_ssa >= register_source_ssa.size()) {
				return false;
			}
			register_source_ssa[result_ssa] = machine_plan_abi(
				ZEND_MIR_REPRESENTATION_I1,
				ZEND_MIR_SCALAR_TYPE_I1,
				ZEND_TPDE_MACHINE_VALUE_BOOL,
				ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE);
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
			if (!instruction.has_value_operation
					|| instruction.value_operation.opcode
						!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					|| (instruction.value_operation.source_opcode
							!= ZEND_JMPZ
						&& instruction.value_operation.source_opcode
							!= ZEND_JMPNZ)) {
				return false;
			}
			const int32_t condition = machine_plan_source_value_index(
				plan, instruction.value_operation.op1);
			const zend_mir_value_id source_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			const bool source_override =
				source_ssa < register_source_ssa.size()
				&& register_source_ssa[source_ssa].valid
				&& register_source_ssa[source_ssa].exact_type
					== ZEND_MIR_SCALAR_TYPE_I1;
			if (!source_override
					&& (condition < 0
						|| plan->values[condition].exact_type
							!= ZEND_MIR_SCALAR_TYPE_I1)) {
				return false;
			}
			continue;
		}
		if (zend_tpde_typed_body_frame_transport(plan, instruction)) {
			continue;
		}
		if (zend_tpde_typed_numeric_binary(plan, instruction)) {
			/* Both operands are register longs or boxed numbers; the result
			 * is a boolean or a boxed number that needs no ownership. */
			auto operand_abi = [&](
					const zend_tpde_source_value_binding &binding,
					const zend_mir_source_operand_ref &operand) {
				zend_tpde_local_abi_type abi{};
				if (binding.value_index >= 0
						&& static_cast<uint32_t>(binding.value_index)
							< plan->value_count) {
					abi = machine_plan_value_abi(plan,
						static_cast<uint32_t>(binding.value_index));
				}
				if (!abi.valid && binding.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							binding.definition_instruction_index)
							< instruction_result_types.size()) {
					abi = instruction_result_types[static_cast<uint32_t>(
						binding.definition_instruction_index)];
				}
				if (!abi.valid
						&& operand.ssa_variable_id
							< register_source_ssa.size()) {
					abi = register_source_ssa[operand.ssa_variable_id];
				}
				return abi.valid
					&& (abi.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64
						|| abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
			};
			if (!operand_abi(instruction.source_op1_binding,
						instruction.value_operation.op1)
					|| !operand_abi(instruction.source_op2_binding,
						instruction.value_operation.op2)) {
				return false;
			}
			const zend_tpde_machine_value_kind result_kind =
				zend_tpde_typed_numeric_result_kind(plan, instruction);
			const zend_tpde_local_abi_type result =
				result_kind == ZEND_TPDE_MACHINE_VALUE_I64
					? machine_plan_abi(ZEND_MIR_REPRESENTATION_I64,
						ZEND_MIR_SCALAR_TYPE_I64, ZEND_TPDE_MACHINE_VALUE_I64,
						ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE)
				: result_kind == ZEND_TPDE_MACHINE_VALUE_BOOL
					? machine_plan_abi(ZEND_MIR_REPRESENTATION_I1,
						ZEND_MIR_SCALAR_TYPE_I1, ZEND_TPDE_MACHINE_VALUE_BOOL,
						ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE)
					: machine_plan_abi(ZEND_MIR_REPRESENTATION_ZVAL,
						ZEND_MIR_SCALAR_TYPE_NONE,
						ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL);
			/* An exact long result cannot overflow: Zend's inference makes
			 * the result of two longs a long only when its range excludes
			 * overflow, so the operation cannot fail the body. */
			instruction_result_types[index] = result;
			register_source_ssa[
				instruction.value_operation.result.ssa_variable_id] = result;
			continue;
		}
		/* A ternary's QM_ASSIGN of a register scalar or number forwards
		 * its source. */
		if (zend_tpde_typed_forward_assign(instruction)
				&& instruction.value_operation.result.ssa_variable_id
					< register_source_ssa.size()) {
			zend_tpde_local_abi_type source{};
			const zend_tpde_source_value_binding &binding =
				instruction.source_op1_binding;
			const int32_t literal = instruction.value_operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				? machine_plan_source_value_index(
					plan, instruction.value_operation.op1)
				: -1;
			if (literal >= 0 && plan->values[literal].constant
					&& (plan->values[literal].exact_type
							== ZEND_MIR_SCALAR_TYPE_I64
						|| plan->values[literal].exact_type
							== ZEND_MIR_SCALAR_TYPE_F64)) {
				source = machine_plan_value_abi(
					plan, static_cast<uint32_t>(literal));
			} else if (zend_tpde_binding_is_number(plan, component_plans,
					component_count, binding)) {
				int32_t producer = binding.definition_instruction_index;
				if (producer < 0 && binding.value_index >= 0
						&& plan->source_value_definition_instructions
							!= nullptr) {
					producer = plan->source_value_definition_instructions[
						binding.value_index];
				}
				source = producer >= 0
						&& static_cast<uint32_t>(producer)
							< instruction_result_types.size()
					? instruction_result_types[
						static_cast<uint32_t>(producer)]
					: zend_tpde_local_abi_type{};
			}
			if (!source.valid) {
				return false;
			}
			instruction_result_types[index] = source;
			register_source_ssa[
				instruction.value_operation.result.ssa_variable_id] = source;
			continue;
		}
		if (record.effects != 0 || record.reads != 0
				|| record.writes != 0 || record.barriers != 0
				|| record.ownership_actions != 0) {
			return false;
		}
		int32_t returned = -1;
		zend_tpde_local_abi_type returned_source_type{};
		if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
			if (instruction.operand_count != 1) {
				return false;
			}
			returned = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, 0));
		} else if (record.opcode
				== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			if (!instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_RETURN) {
				return false;
			}
			returned = instruction.source_op1_binding.value_index;
			const zend_mir_value_id returned_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			if (instruction.source_op1_binding
							.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)
						< instruction_result_types.size()) {
				returned_source_type = instruction_result_types[
					static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)];
			}
			if (!returned_source_type.valid
					&& returned_ssa < register_source_ssa.size()) {
				returned_source_type = register_source_ssa[returned_ssa];
			}
		} else {
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				for (uint32_t operand = 0;
						operand < instruction.operand_count; ++operand) {
					if (!value_has_typed_body_definition(
							zend_tpde_value_index(plan,
								zend_tpde_operand_at(
									plan, &instruction, operand)))) {
						return false;
					}
				}
			}
			const bool pure =
				record.opcode == ZEND_MIR_OPCODE_CONSTANT
				|| record.opcode == ZEND_MIR_OPCODE_PHI
				|| record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
				|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				|| record.opcode == ZEND_MIR_OPCODE_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_UNREACHABLE
				|| (record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
					&& record.opcode <= ZEND_MIR_OPCODE_SCALAR_DROP);
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (!pure
					|| (record.representation
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& record.result_id != ZEND_MIR_ID_INVALID
						&& result_index >= 0
						&& !machine_plan_value_abi(
							plan, static_cast<uint32_t>(
								result_index)).valid
						&& !(record.opcode
							== ZEND_MIR_OPCODE_CONSTANT
							&& plan->values[result_index].exact_type
								== ZEND_MIR_SCALAR_TYPE_NULL))) {
				return false;
			}
			continue;
		}
		if (returned < 0 && !returned_source_type.valid) {
			return false;
		}
		zend_tpde_local_abi_type current_return =
			returned_source_type.valid
				? returned_source_type
				: machine_plan_value_abi(
					plan, static_cast<uint32_t>(returned));
		if (returned >= 0 && !current_return.valid) {
			current_return =
				call_result_types[static_cast<uint32_t>(returned)];
		}
		/* A number returned as int may fail: a double needs Zend's coercion
		 * or TypeError. As float it converts. */
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& zend_tpde_machine_kind_is_number(
					plan->return_abi.machine_kind)
				&& zend_mir_scalar_type_is_exact(plan->return_abi.exact_type)
				&& current_return.valid
				&& current_return.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& zend_tpde_binding_is_number(plan, component_plans,
					component_count, instruction.source_op1_binding)) {
			*may_fail = *may_fail || plan->return_abi.machine_kind
				== ZEND_TPDE_MACHINE_VALUE_I64;
			current_return = plan->return_abi;
		}
		/* A scalar returned through an untyped result is boxed at the
		 * return. */
		if (current_return.valid
				&& zend_mir_scalar_type_is_exact(current_return.exact_type)
				&& current_return.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& plan->return_abi.valid
				&& plan->return_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			current_return = plan->return_abi;
		}
		if (!current_return.valid
				|| (saw_return
					&& !machine_plan_abi_same_shape(
						result_type, current_return))) {
			return false;
		}
		result_type = current_return;
		saw_return = true;
	}
	if (!saw_return
			|| !machine_plan_abi_same_shape(
				result_type, plan->return_abi)) {
		return false;
	}
	*return_type = result_type;
	return true;
}

/*
 * A proof-closed scalar body is cloned rather than called through the local
 * component ABI. The adaptor makes the final value-mapping proof; this
 * structural screen admits exactly the instructions the ZNMIR cloner
 * accepts, so the machine CFG keeps the inline-capable form.
 */
static bool machine_plan_prefers_effect_closed_inline(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee) {
	zend_tpde_scalar_diamond diamond{};
	if (call.direct_call == nullptr || callee == nullptr
			|| (call.direct_call->receiver_kind
					!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
				&& (call.direct_call->receiver_kind
						!= ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
					|| (call.direct_call->flags
							& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0))
			|| (call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I1
				&& call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I64)
			|| callee->block_ids == nullptr
			|| callee->generator_resume_count != 0
			|| callee->user_opcode_callbacks) {
		return false;
	}
	const bool scalar_diamond = callee->block_count != 1;
	if ((callee->block_count != 1
			&& (!zend_tpde_scalar_diamond_at(callee, &diamond)))) {
		return false;
	}

	bool saw_return = false;
	uint32_t checked_binary_count = 0;
	bool saw_string_length = false;
	uint32_t semantic_instruction_count = 0;
	uint32_t conditional_branch_count = 0;
	uint32_t arm_branch_count = 0;
	uint32_t phi_count = 0;
	for (uint32_t index = 0; index < callee->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			callee->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(callee, &instruction);
		const int32_t block_index =
			zend_tpde_block_index(callee, record.block_id);
		if (block_index < 0
				|| (!scalar_diamond
					&& record.block_id != callee->block_ids[0])) {
			return false;
		}
		if (instruction.local_abi_transport) {
			continue;
		}
		if (scalar_diamond
				&& zend_tpde_scalar_diamond_frame_transport(
					callee, instruction)) {
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
			if (record.effects != 0) {
				return false;
			}
			continue;
		}
		if (++semantic_instruction_count > 24) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_PHI) {
			if (!scalar_diamond
					|| static_cast<uint32_t>(block_index) != diamond.merge
					|| !zend_mir_id_is_valid(record.result_id)
					|| instruction.operand_count != 2) {
				return false;
			}
			++phi_count;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH) {
			if (!scalar_diamond
					|| static_cast<uint32_t>(block_index) != diamond.entry
					|| instruction.operand_count != 1
					|| ++conditional_branch_count != 1) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_BRANCH) {
			const uint32_t block = static_cast<uint32_t>(block_index);
			if (!scalar_diamond
					|| (block != diamond.true_block
						&& block != diamond.false_block)
					|| ++arm_branch_count > 2) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
				&& (instruction.runtime_helper
						== ZEND_NATIVE_HELPER_COUNT
					|| saw_string_length
					|| (checked_binary_count != 0
						&& call.direct_call->result_type
							== ZEND_MIR_SCALAR_TYPE_I64)
					|| (callee->typed_body_return_abi.valid
						&& callee->typed_body_return_abi.exact_type
							== call.direct_call->result_type))) {
			/*
			 * The typed-body signature, derived from every return before
			 * component calls are classified, proves the cloned body cannot
			 * take the coercion or TypeError path of the Zend entry.
			 */
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			if (saw_return || !instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_RETURN
					|| (scalar_diamond
						&& static_cast<uint32_t>(block_index)
							!= diamond.merge)) {
				return false;
			}
			saw_return = true;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
			if (saw_return || instruction.operand_count != 1
					|| (scalar_diamond
						&& static_cast<uint32_t>(block_index)
							!= diamond.merge)) {
				return false;
			}
			saw_return = true;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE) {
			if (!zend_mir_id_is_valid(record.result_id)
					|| instruction.operand_count != 1) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
			const uint32_t checked_binary_limit =
				8;
			if (scalar_diamond
					|| checked_binary_count >= checked_binary_limit
					|| !instruction.has_value_operation
					|| (instruction.value_operation.source_opcode
							!= ZEND_ADD
						&& instruction.value_operation.source_opcode
							!= ZEND_SUB)
					|| call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64) {
				return false;
			}
			++checked_binary_count;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
			if (scalar_diamond || checked_binary_count != 0 || saw_string_length
					|| !instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_STRLEN
					|| call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64) {
				return false;
			}
			saw_string_length = true;
			continue;
		}
		const bool integer_or_boolean_opcode =
			(record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
				&& record.opcode <= ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW)
			|| (record.opcode >= ZEND_MIR_OPCODE_I64_BIT_OR
				&& record.opcode <= ZEND_MIR_OPCODE_I64_CMP)
			|| record.opcode == ZEND_MIR_OPCODE_I1_NOT
			|| record.opcode == ZEND_MIR_OPCODE_I1_XOR
			|| record.opcode == ZEND_MIR_OPCODE_I1_EQ
			|| record.opcode == ZEND_MIR_OPCODE_I64_TO_I1
			|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64;
		if (checked_binary_count != 0 || saw_string_length
				|| !integer_or_boolean_opcode
				|| record.effects != 0
				|| !zend_mir_id_is_valid(record.result_id)
				|| instruction.operand_count > 2) {
			return false;
		}
	}
	return saw_return
		&& (!scalar_diamond
			|| (conditional_branch_count == 1
				&& arm_branch_count == 2 && phi_count != 0));
}

/*
 * Add the arguments a member's body reads to those its receive checks: an
 * operand or source binding whose register alias chain reaches the
 * argument's value. A chain that leaves the value table or does not end
 * counts as reading every argument.
 */
static void freeze_effect_closed_argument_use(zend_tpde_plan *plan) {
	if (plan->argument_count == 0
			|| plan->effect_closed_argument_used == nullptr) {
		return;
	}
	uint8_t *used = plan->effect_closed_argument_used;
	if (plan->argument_value_indices == nullptr) {
		std::fill_n(used, plan->argument_count, 1);
		return;
	}
	std::vector<uint8_t> argument_value(plan->value_count, 0);
	for (uint32_t argument = 0; argument < plan->argument_count; ++argument) {
		const int32_t value = plan->argument_value_indices[argument];
		if (value < 0 || static_cast<uint32_t>(value) >= plan->value_count) {
			used[argument] = 1;
		} else {
			argument_value[static_cast<uint32_t>(value)] = 1;
		}
	}
	bool all_used = false;
	auto mark = [&](int32_t value_index) {
		for (uint32_t depth = 0;
				value_index >= 0 && depth < plan->value_count;
				++depth) {
			if (static_cast<uint32_t>(value_index) >= plan->value_count) {
				all_used = true;
				return;
			}
			if (argument_value[static_cast<uint32_t>(value_index)] != 0) {
				for (uint32_t argument = 0;
						argument < plan->argument_count; ++argument) {
					if (plan->argument_value_indices[argument]
							== value_index) {
						used[argument] = 1;
					}
				}
			}
			const int32_t alias =
				plan->values[static_cast<uint32_t>(value_index)]
					.register_alias_value_index;
			if (alias < 0 || alias == value_index) {
				return;
			}
			value_index = alias;
		}
		if (value_index >= 0) {
			all_used = true;
		}
	};
	for (uint32_t index = 0;
			!all_used && index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			mark(zend_tpde_value_index(plan,
				zend_tpde_operand_at(plan, &instruction, operand)));
		}
		mark(instruction.source_op1_binding.value_index);
		mark(instruction.source_op2_binding.value_index);
		mark(instruction.source_auxiliary_binding.value_index);
	}
	if (all_used) {
		std::fill_n(used, plan->argument_count, 1);
	}
}

static bool machine_plan_effect_closed_argument_is_used(
		const zend_tpde_plan *callee, uint32_t argument) {
	return callee == nullptr || argument >= callee->argument_count
		|| callee->effect_closed_argument_used == nullptr
		|| callee->effect_closed_argument_used[argument] != 0;
}

static bool machine_value_kind_can_be_register_authoritative(
		zend_tpde_machine_value_kind kind) {
	return zend_tpde_machine_kind_is_pointer(kind)
		|| kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
}

[[nodiscard]]
static bool freeze_typed_component_calls(
		zend_tpde_plan *plan,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count) {
	plan->has_register_component_results = false;
	if (plan->instruction_count == 0) {
		return true;
	}
	plan->typed_component_call_eligible = static_cast<uint8_t *>(
		std::calloc(plan->instruction_count, sizeof(uint8_t)));
	plan->effect_closed_inline_eligible = static_cast<uint8_t *>(
		std::calloc(plan->instruction_count, sizeof(uint8_t)));
	if (plan->typed_component_call_eligible == nullptr
			|| plan->effect_closed_inline_eligible == nullptr) {
		return false;
	}
	if (plan->call_argument_bindings == nullptr) {
		return true;
	}
	std::vector<zend_tpde_local_abi_type> instruction_result_types(
		plan->instruction_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (result_index >= 0) {
				instruction_result_types[index] =
					machine_plan_value_abi(
						plan, static_cast<uint32_t>(result_index));
			}
			continue;
		}
		const uint32_t target = instruction.component_target_index;
		const zend_tpde_plan *callee =
			target < component_count ? component_plans[target] : nullptr;
		if (instruction.direct_call == nullptr
				|| callee == nullptr
				|| instruction.direct_call->expected_function == nullptr
				|| (instruction.direct_call->expected_function->common.fn_flags
					& (ZEND_ACC_DEPRECATED | ZEND_ACC_NODISCARD)) != 0
				|| (instruction.entry_cell != nullptr
					&& instruction.entry_cell->function != nullptr
					&& (instruction.entry_cell->function->common.fn_flags
						& (ZEND_ACC_DEPRECATED | ZEND_ACC_NODISCARD)) != 0)
				|| instruction.call_argument_count
					!= callee->argument_count
				|| (callee->argument_count != 0
					&& callee->argument_value_indices == nullptr)
				|| instruction.call_argument_offset
					> plan->call_argument_count
				|| instruction.call_argument_count
					> plan->call_argument_count
						- instruction.call_argument_offset) {
			continue;
		}
		const bool effect_closed_inline =
			machine_plan_prefers_effect_closed_inline(
				instruction, callee)
			&& !zend_tpde_direct_call_has_argument_guard(instruction);
		bool compatible = true;
		for (uint32_t argument = 0;
				argument < instruction.call_argument_count; ++argument) {
			if (effect_closed_inline
					&& !machine_plan_effect_closed_argument_is_used(
						callee, argument)) {
				continue;
			}
			zend_mir_call_argument_ref source_argument{};
			const uint32_t argument_index =
				instruction.call_argument_offset + argument;
			const int32_t callee_value =
				callee->argument_value_indices[argument];
			if (!zend_tpde_call_argument_at(
					plan, argument_index, &source_argument)
					|| callee_value < 0
					|| static_cast<uint32_t>(callee_value)
						>= callee->value_count) {
				compatible = false;
				break;
			}
			const zend_tpde_source_value_binding &binding =
				plan->call_argument_bindings[argument_index];
			zend_tpde_local_abi_type caller_abi{};
			const int32_t source_value = machine_plan_source_value_index(
				plan, source_argument.source_operand);
			if (source_value >= 0) {
				caller_abi = machine_plan_value_abi(
					plan, static_cast<uint32_t>(source_value));
			}
			if (!caller_abi.valid && binding.value_index >= 0
					&& static_cast<uint32_t>(binding.value_index)
						< plan->value_count) {
				caller_abi = machine_plan_value_abi(
					plan, static_cast<uint32_t>(binding.value_index));
			}
			if (!caller_abi.valid
					&& binding.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						binding.definition_instruction_index)
						< instruction_result_types.size()) {
				caller_abi = instruction_result_types[
					static_cast<uint32_t>(
						binding.definition_instruction_index)];
			}
			/* A proven arithmetic result, or the result of a member that
			 * returns numbers, owns nothing. */
			const bool number_binding = zend_tpde_binding_is_number(
				plan, component_plans, component_count, binding);
			if (number_binding
					&& (!caller_abi.valid
						|| caller_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
				caller_abi = machine_plan_abi(
					ZEND_MIR_REPRESENTATION_ZVAL,
					ZEND_MIR_SCALAR_TYPE_NONE,
					ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
					ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL);
			}
			const zend_tpde_local_abi_type callee_abi =
				machine_plan_value_abi(
					callee, static_cast<uint32_t>(callee_value));
			/* A typed call skips the receive check: a parameter with a
			 * declared type needs an argument that already satisfies it. */
			if (argument >= 32
					? callee->checked_boxed_argument_mask != 0
					: ((callee->checked_boxed_argument_mask >> argument) & 1)
						!= 0) {
				const bool number_argument =
					caller_abi.valid
					&& (number_binding
						|| caller_abi.exact_type == ZEND_MIR_SCALAR_TYPE_I64
						|| caller_abi.exact_type == ZEND_MIR_SCALAR_TYPE_F64
						|| (source_argument.send_opline_index
								< plan->source_opcode_count
							&& zend_tpde_known_numeric_type(
								plan->source_opcodes[
									source_argument.send_opline_index]
									.op1_known_type)));
				if (argument >= 32 || !number_argument
						|| ((callee->number_argument_mask >> argument) & 1)
							== 0) {
					compatible = false;
					break;
				}
			}
			const zend_tpde_instruction *boxed_read_producer =
				binding.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							binding.definition_instruction_index)
							< plan->instruction_count
					? &plan->instructions[static_cast<uint32_t>(
						binding.definition_instruction_index)]
					: nullptr;
			const zend_tpde_machine_reference *boxed_read_reference =
				boxed_read_producer != nullptr
						&& boxed_read_producer->operation_reference_index
							< plan->machine_reference_count
					? &plan->machine_references[
						boxed_read_producer->operation_reference_index]
					: nullptr;
			zend_tpde_object_property_read property_layout{};
			zend_tpde_array_read array_layout{};
			const bool guarded_property_pointer_transport =
				boxed_read_producer != nullptr
				&& boxed_read_producer->record.opcode
					== ZEND_MIR_OPCODE_OBJECT_FETCH_R
				&& zend_tpde_object_property_read_at(
					*boxed_read_producer, &property_layout)
				&& boxed_read_reference != nullptr
				&& boxed_read_reference->kind
					== ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT
				&& boxed_read_reference->stable_storage_or_layout_id
					== property_layout.cache_offset
				&& boxed_read_reference->access_width == sizeof(zval);
			const bool guarded_array_pointer_transport =
				boxed_read_producer != nullptr
				&& boxed_read_producer->record.opcode
					== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				&& zend_tpde_array_read_at(
					*boxed_read_producer, &array_layout,
					zend_tpde_linux_inline_forms)
				&& boxed_read_reference != nullptr
				&& boxed_read_reference->kind
					== ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT
				&& zend_mir_id_is_valid(
					boxed_read_reference->base_value_id)
				&& zend_mir_id_is_valid(
					boxed_read_reference->index_value_id)
				&& boxed_read_reference->scale == sizeof(zval)
				&& boxed_read_reference->access_width == sizeof(zval);
			const bool guarded_boxed_pointer_transport =
				!effect_closed_inline
				&& callee_abi.valid
				&& machine_value_kind_can_be_register_authoritative(
					callee_abi.machine_kind)
				&& callee_abi.representation
					== ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				&& (guarded_property_pointer_transport
					|| guarded_array_pointer_transport)
				&& argument < instruction.direct_call->argument_count
				&& instruction.direct_call->arguments[argument].exact_type
					== callee_abi.exact_type
				&& source_argument.ownership
					!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			if (guarded_boxed_pointer_transport) {
				/*
				 * A property or array read produces a two-part register zval.
				 * The AArch64 entry guard checks its type before the hot block
				 * extracts the refcounted payload, which is then the callee's
				 * borrowed pointer; the cold call keeps Zend semantics.
				 */
				caller_abi = callee_abi;
			}
			const zend_tpde_value *guarded_integer_source =
				source_value >= 0
						&& static_cast<uint32_t>(source_value)
							< plan->value_count
					? &plan->values[static_cast<uint32_t>(source_value)]
					: nullptr;
			const int32_t guarded_integer_definition =
				binding.definition_instruction_index;
			const zend_mir_instruction_record guarded_integer_definition_record =
				guarded_integer_definition >= 0
						&& static_cast<uint32_t>(guarded_integer_definition)
							< plan->instruction_count
					? zend_tpde_instruction_record_at(plan,
						&plan->instructions[static_cast<uint32_t>(
							guarded_integer_definition)])
					: zend_mir_instruction_record{};
			const bool guarded_boxed_integer_transport =
				!effect_closed_inline
				&& guarded_integer_source != nullptr
				&& guarded_integer_source->representation
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& guarded_integer_source->machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& guarded_integer_definition >= 0
				&& guarded_integer_definition_record.opcode
					!= ZEND_MIR_OPCODE_COPY
				&& guarded_integer_definition_record.opcode
					!= ZEND_MIR_OPCODE_PHI
				&& zend_mir_id_is_valid(
					guarded_integer_source->canonical_storage_id)
				&& callee_abi.valid
				&& callee_abi.representation == ZEND_MIR_REPRESENTATION_I64
				&& callee_abi.exact_type == ZEND_MIR_SCALAR_TYPE_I64
				&& callee_abi.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64
				&& argument < instruction.direct_call->argument_count
				&& instruction.direct_call->arguments[argument].mode
					== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
				&& instruction.direct_call->arguments[argument].exact_type
					== ZEND_MIR_SCALAR_TYPE_I64
				&& instruction.direct_call_argument_guard_types != nullptr
				&& instruction.direct_call_argument_guard_types[argument]
					== ZEND_MIR_SCALAR_TYPE_I64;
			if (guarded_boxed_integer_transport) {
				/*
				 * The descriptor's IS_LONG guard for this boxed argument lets
				 * the typed hot edge pass the payload as the callee's I64.
				 */
				caller_abi = callee_abi;
			}
			if (effect_closed_inline
					&& source_value >= 0
					&& static_cast<uint32_t>(source_value)
						< plan->value_count) {
				const zend_tpde_value &source =
					plan->values[static_cast<uint32_t>(source_value)];
				/*
				 * A canonical refcounted value reaches an effect-closed clone
				 * by one payload load; its slot stays authoritative, and the
				 * call needs no private Zend frame.
				 */
				if (!caller_abi.valid
						&& source.machine_kind == callee_abi.machine_kind
						&& zend_mir_id_is_valid(
							source.canonical_storage_id)
						&& machine_value_kind_can_be_register_authoritative(
							source.machine_kind)) {
					caller_abi = callee_abi;
				} else if (source.representation
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& source.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& (source.exact_type
								== callee_abi.exact_type
							|| (source.exact_type
									== ZEND_MIR_SCALAR_TYPE_NONE
								&& argument
									< instruction.direct_call
										->argument_count
								&& instruction.direct_call
										->arguments[argument].exact_type
									== callee_abi.exact_type))
						&& zend_tpde_machine_kind_is_long_or_bool(
							callee_abi.machine_kind)
						&& zend_mir_id_is_valid(
							source.canonical_storage_id)) {
					caller_abi = callee_abi;
				} else if (!caller_abi.valid
						&& source.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
						&& callee_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						&& argument < instruction.direct_call->argument_count
						&& instruction.direct_call->arguments[argument].exact_type
							== callee_abi.exact_type
						&& zend_mir_id_is_valid(source.canonical_storage_id)
						&& source_argument.send_opline_index
							< plan->source_opcode_count
						&& plan->source_opcodes[
							source_argument.send_opline_index].op1_type == IS_CV) {
					/*
					 * By-value SEND dereferences a reference-cell CV; with the
					 * payload type the descriptor fixed, the clone does the
					 * same load.
					 */
					caller_abi = callee_abi;
				}
			}
			const bool by_reference =
				source_argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			const bool guarded_boxed_integer_required =
				instruction.direct_call_argument_guard_types != nullptr
				&& instruction.direct_call_argument_guard_types[argument]
					== ZEND_MIR_SCALAR_TYPE_I64
				&& guarded_integer_source != nullptr
				&& guarded_integer_source->representation
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& guarded_integer_source->machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
			if ((guarded_boxed_integer_required
					&& !guarded_boxed_integer_transport)
					|| !machine_plan_call_argument_can_supply(
					plan, source_argument, caller_abi, callee_abi)
					|| by_reference
						!= (callee_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
				compatible = false;
				break;
			}
		}
		if (!compatible) {
			continue;
		}
		if (effect_closed_inline) {
			plan->effect_closed_inline_eligible[index] = 1;
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			plan->has_register_component_results = true;
			continue;
		}
		if (instruction.direct_call->receiver_kind
				!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE) {
			continue;
		}
		if (!callee->typed_body_eligible
				|| !callee->typed_body_return_abi.valid) {
			continue;
		}
		plan->typed_component_call_eligible[index] = 1;
		instruction.component_body_function_index =
			callee->typed_body_function_index;
		instruction_result_types[index] =
			callee->typed_body_return_abi;
		plan->has_register_component_results = true;
	}
	return true;
}

/*
 * Consumers of a slot-authoritative scalar load it from its canonical
 * slot, so a lazy store of one must write the slot.  Lazy stores imply a
 * non-counted previous value, which makes the plain form exact.
 */
static void demote_slot_backed_lazy_stores(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		if (!instruction.zval_store_lazy_scalar
				|| instruction.operand_count < 1) {
			continue;
		}
		const int32_t source = zend_tpde_value_index(
			plan, zend_tpde_operand_at(plan, &instruction, 0));
		if (source < 0
				|| (!plan->values[source].constant
					&& !plan->values[source].register_authoritative)) {
			instruction.zval_store_lazy_scalar = false;
			instruction.zval_store_plain = true;
		}
	}
}

static bool machine_plan_value_has_result_representation(
		const zend_tpde_plan *plan, zend_mir_value_id value_id) {
	const int32_t value_index = zend_tpde_value_index(plan, value_id);
	if (value_index < 0) {
		return false;
	}
	const zend_tpde_value &value =
		plan->values[static_cast<uint32_t>(value_index)];
	const int32_t definition =
		plan->value_definition_instructions == nullptr
			? -1 : plan->value_definition_instructions[value_index];
	/* The adaptor defines a transient scalar result in registers. */
	if (definition >= 0
			&& static_cast<uint32_t>(definition) < plan->instruction_count
			&& plan->instructions[static_cast<uint32_t>(definition)]
				.transient_scalar_result) {
		return true;
	}
	return zend_tpde_machine_result_representation(value.exact_type,
		value.machine_kind, value.constant, value.register_authoritative);
}

[[nodiscard]]
static bool retain_typed_call_materializations(
		zend_tpde_plan *plan,
		zend_native_diagnostic *diag) {
	if (plan->materialization_count == 0) {
		return true;
	}
	std::vector<zend_mir_storage_id> lazy_scalar_storages;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_storage_id storage_id =
			instruction.zval_store_lazy_scalar
				? instruction.zval_store_storage_id
				: instruction.mutation_lazy_scalar
					? instruction.mutation_storage_id
					: ZEND_MIR_ID_INVALID;
		if (zend_mir_id_is_valid(storage_id)) {
			lazy_scalar_storages.push_back(storage_id);
		}
	}
	std::ranges::sort(lazy_scalar_storages);
	lazy_scalar_storages.erase(
		std::unique(lazy_scalar_storages.begin(), lazy_scalar_storages.end()),
		lazy_scalar_storages.end());
	/* A COPY or PHI joins a machine source directly or through other
	 * joins, such as an outer loop's PHI over an inner loop's. Each search
	 * marks its visits with its own generation. */
	std::vector<uint32_t> join_visited(plan->value_count, 0);
	uint32_t join_generation = 0;
	auto lazy_join_has_machine_source = [&](uint32_t value_index,
			auto &&self) -> bool {
		if (value_index >= plan->value_count
				|| join_visited[value_index] == join_generation) {
			return false;
		}
		join_visited[value_index] = join_generation;
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_opcode opcode =
			zend_tpde_instruction_record_at(plan, &instruction).opcode;
		if (opcode != ZEND_MIR_OPCODE_COPY
				&& opcode != ZEND_MIR_OPCODE_PHI) {
			return false;
		}
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			const zend_mir_value_id operand_id =
				zend_tpde_operand_at(plan, &instruction, operand);
			if (machine_plan_value_has_result_representation(
						plan, operand_id)) {
				return true;
			}
			const int32_t operand_index =
				zend_tpde_value_index(plan, operand_id);
			if (operand_index >= 0
					&& plan->values[operand_index].canonical_storage_id
						== plan->values[value_index].canonical_storage_id
					&& self(static_cast<uint32_t>(operand_index), self)) {
				return true;
			}
		}
		return false;
	};
	/* The answer per searched value: 0 unknown, 1 no, 2 yes. */
	std::vector<uint8_t> join_has_machine_source(plan->value_count, 0);
	auto lazy_join_root_has_machine_source = [&](uint32_t value_index) {
		if (value_index >= plan->value_count) {
			return false;
		}
		if (join_has_machine_source[value_index] == 0) {
			++join_generation;
			join_has_machine_source[value_index] =
				lazy_join_has_machine_source(
					value_index, lazy_join_has_machine_source) ? 2 : 1;
		}
		return join_has_machine_source[value_index] == 2;
	};
	std::vector<zend_tpde_materialization> retained;
	retained.reserve(plan->materialization_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const uint32_t old_offset = instruction.materialization_offset;
		const uint32_t old_count = instruction.materialization_count;
		instruction.materialization_offset =
			static_cast<uint32_t>(retained.size());
		instruction.materialization_count = 0;
		if (old_offset > plan->materialization_count
				|| old_count > plan->materialization_count - old_offset) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"component materialization slice is out of bounds");
			return false;
		}
		for (uint32_t offset = 0; offset < old_count; ++offset) {
			const zend_tpde_materialization &materialization =
				plan->materializations[old_offset + offset];
			const bool lazy_scalar = std::binary_search(
				lazy_scalar_storages.begin(), lazy_scalar_storages.end(),
				materialization.storage_id);
			if (materialization.value_index == UINT32_MAX) {
				const int32_t definition =
					materialization.source_definition_instruction_index;
				if (definition < 0
						|| static_cast<uint32_t>(definition)
							>= plan->instruction_count) {
					continue;
				}
				const uint32_t definition_index =
					static_cast<uint32_t>(definition);
				const zend_tpde_instruction &definition_instruction =
					plan->instructions[definition_index];
				const bool machine_definition =
					(lazy_scalar
						&& definition_instruction.mutation_lazy_scalar)
					|| (definition_instruction.has_value_operation
						&& definition_instruction.value_operation.source_opcode
							== ZEND_STRLEN)
					|| (plan->typed_component_call_eligible != nullptr
						&& plan->typed_component_call_eligible[
							definition_index] != 0)
					|| (plan->effect_closed_inline_eligible != nullptr
						&& plan->effect_closed_inline_eligible[
							definition_index] != 0);
				if (!machine_definition) {
					continue;
				}
			} else {
				if (materialization.value_index >= plan->value_count) {
					continue;
				}
				const zend_tpde_value &value =
					plan->values[materialization.value_index];
				const int32_t definition =
					plan->value_definition_instructions == nullptr
						? -1
						: plan->value_definition_instructions[
							materialization.value_index];
				const zend_mir_opcode definition_opcode =
					definition >= 0
						&& static_cast<uint32_t>(definition)
							< plan->instruction_count
					? zend_tpde_instruction_record_at(
						plan, &plan->instructions[
							static_cast<uint32_t>(definition)]).opcode
					: ZEND_MIR_OPCODE_INVALID;
				const bool lazy_scalar_join = lazy_scalar
					&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& !value.constant
					&& !value.canonical_alias_observable
					&& (definition_opcode == ZEND_MIR_OPCODE_COPY
						|| definition_opcode == ZEND_MIR_OPCODE_PHI)
					&& lazy_join_root_has_machine_source(
						materialization.value_index);
				if (!lazy_scalar_join
						&& !machine_plan_value_has_result_representation(
							plan, value.id)) {
					continue;
				}
			}
			retained.push_back(materialization);
			++instruction.materialization_count;
		}
	}

	zend_tpde_materialization *replacement = nullptr;
	if (!retained.empty()) {
		replacement = static_cast<zend_tpde_materialization *>(
			std::malloc(retained.size() * sizeof(*replacement)));
		if (replacement == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to retain component materialization plan");
			return false;
		}
		std::memcpy(replacement, retained.data(),
			retained.size() * sizeof(*replacement));
	}
	std::free(plan->materializations);
	plan->materializations = replacement;
	plan->materialization_count =
		static_cast<uint32_t>(retained.size());
	return true;
}

void destroy_machine_cfg(zend_tpde_machine_cfg *cfg) {
	std::free(cfg->successor_offsets);
	std::free(cfg->successors);
	std::free(cfg->instruction_blocks);
	std::free(cfg->guarded_cold_blocks);
	std::free(cfg->guarded_hot_blocks);
	std::free(cfg->guarded_continuation_blocks);
	std::free(cfg->final_blocks);
	std::free(cfg->boxed_cond_cold_blocks);
	std::free(cfg->boxed_cond_cold_by_predecessor);
	std::memset(cfg, 0, sizeof(*cfg));
}

[[nodiscard]]
static bool freeze_machine_cfg_array(
		uint32_t **out,
		const std::vector<uint32_t> &values) {
	if (values.empty()) {
		*out = nullptr;
		return true;
	}
	*out = static_cast<uint32_t *>(
		std::malloc(values.size() * sizeof(**out)));
	if (*out == nullptr) {
		return false;
	}
	std::memcpy(*out, values.data(), values.size() * sizeof(**out));
	return true;
}

static int32_t machine_cfg_register_cond_branch_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	int32_t value_index = instruction.source_op1_binding.value_index;
	if (value_index >= 0) {
		return value_index;
	}
	zend_mir_value_id value_id;
	if (!source_operand_value_id(
			instruction.value_operation.op1, value_id)) {
		return -1;
	}
	return zend_tpde_value_index(plan, value_id);
}

static bool machine_cfg_register_cond_branch(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction,
		bool typed_body) {
	if ((instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) == 0) {
		return false;
	}
	if ((instruction.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE) != 0) {
		return true;
	}
	const int32_t value_index =
		machine_cfg_register_cond_branch_value_index(plan, instruction);
	if (value_index < 0) {
		return typed_body
			&& instruction.value_operation.op1.ssa_variable_id
				!= ZEND_MIR_ID_INVALID;
	}
	const zend_tpde_value &value =
		plan->values[static_cast<uint32_t>(value_index)];
	const bool machine_condition =
		(value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
			&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL)
		|| (value.exact_type == ZEND_MIR_SCALAR_TYPE_I64
			&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64);
	if (machine_condition
			&& (typed_body || value.register_authoritative)) {
		return true;
	}
	/*
	 * Entry isset/empty and BOOL chains get their I1 result in the adaptor
	 * while their source value stays a zval: recognize the register-result
	 * producer instead.
	 */
	if (typed_body || plan->value_definition_instructions == nullptr) {
		return false;
	}
	int32_t definition =
		instruction.source_op1_binding.definition_instruction_index;
	if (definition < 0) {
		definition = plan->value_definition_instructions[
			static_cast<uint32_t>(value_index)];
	}
	if (definition < 0) {
		definition = plan->source_value_definition_instructions == nullptr
			? -1
			: plan->source_value_definition_instructions[
				static_cast<uint32_t>(value_index)];
	}
	if (definition < 0
			|| static_cast<uint32_t>(definition) >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &producer =
		plan->instructions[static_cast<uint32_t>(definition)];
	if ((producer.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0) {
		return false;
	}
	const zend_mir_instruction_record producer_record =
		zend_tpde_instruction_record_at(plan, &producer);
	if (producer_record.opcode == ZEND_MIR_OPCODE_PHI) {
		return true;
	}
	if (producer_record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& producer.direct_call != nullptr
			&& producer.direct_call->result_type
				== ZEND_MIR_SCALAR_TYPE_I1
			&& ((plan->typed_component_call_eligible != nullptr
					&& plan->typed_component_call_eligible[
						static_cast<uint32_t>(definition)] != 0)
				|| (plan->effect_closed_inline_eligible != nullptr
					&& plan->effect_closed_inline_eligible[
						static_cast<uint32_t>(definition)] != 0))) {
		return true;
	}
	if (!producer.has_value_operation) {
		return false;
	}
	return producer_record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
		|| (producer_record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
			&& (producer.value_operation.source_opcode == ZEND_BOOL
				|| producer.value_operation.source_opcode == ZEND_BOOL_NOT));
}

static bool machine_plan_value_needs_result_assignment(
		const zend_tpde_plan *plan, zend_mir_value_id value_id) {
	const int32_t value_index = zend_tpde_value_index(plan, value_id);
	return value_index >= 0
		&& !plan->values[static_cast<uint32_t>(value_index)].constant
		&& machine_plan_value_has_result_representation(plan, value_id);
}

static int32_t machine_plan_guarded_mutation_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	zend_tpde_long_assign_op long_assign{};
	zend_tpde_long_incdec long_incdec{};
	if (!instruction.has_value_operation
			|| !instruction.mutation_lazy_scalar
			|| !((zend_tpde_long_assign_op_at(
						instruction, &long_assign)
						&& !long_assign.has_result)
					|| (zend_tpde_long_incdec_at(
						instruction, &long_incdec)
						&& !long_incdec.has_result))
			|| instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one == 0) {
		return -1;
	}
	return zend_tpde_value_index(
		plan, zend_mir_value_from_original_ssa(
			instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one - 1));
}

static void freeze_machine_scalar_definitions(zend_tpde_plan *plan) {
	if (plan->values == nullptr
			|| plan->instructions == nullptr) {
		return;
	}

	/*
	 * Scalar MIR opcodes define their descriptor's machine payload even when
	 * the source SSA value is a canonical ZVAL. COPY and PHI propagate it to
	 * a fixed point, so loop-carried scalars get backedge definitions instead
	 * of reloads of the loop's initial slot.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[index]);
		const zend_mir_scalar_descriptor *descriptor =
			zend_mir_scalar_descriptor_at(record.opcode);
		if (descriptor == nullptr || !descriptor->has_result
				|| !zend_mir_scalar_type_is_exact(
				descriptor->result.exact_type)
				|| descriptor->result.exact_type
					== ZEND_MIR_SCALAR_TYPE_NULL) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0) {
			continue;
		}
		zend_tpde_value &result =
			plan->values[static_cast<uint32_t>(result_index)];
		if (result.constant || result.canonical_alias_observable) {
			continue;
		}
		result.representation = descriptor->result.representation;
		result.exact_type = descriptor->result.exact_type;
		result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
		result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
		zend_tpde_apply_machine_value_facts(&result, true);
	}

	auto scalar_machine_definition = [](const zend_tpde_value &value) {
		return value.constant
			|| (value.representation != ZEND_MIR_REPRESENTATION_ZVAL
				&& zend_mir_scalar_type_is_exact(value.exact_type)
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& (value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER
					|| (value.argument_index >= 0
						&& value.local_abi.valid)));
	};

	/*
	 * A deferred COPY/PHI input is safe only when its whole same-slot
	 * dependency graph consists of scalar-selectable COPY/PHI values.
	 * Candidates are pruned back from boxed or pointer boundaries, so one
	 * scalar seed cannot promote half of a mixed scalar/zval loop cycle.
	 */
	std::vector<uint8_t> scalar_copy_phi_candidates(plan->value_count);
	for (uint32_t value_index = 0;
			value_index < plan->value_count; ++value_index) {
		const zend_tpde_value &value = plan->values[value_index];
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count
				|| value.constant || value.canonical_alias_observable
				|| value.representation != ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if ((record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_PHI)
				&& record.representation == ZEND_MIR_REPRESENTATION_ZVAL
				&& instruction.operand_count != 0) {
			scalar_copy_phi_candidates[value_index] = 1;
		}
	}

	/* Dropping candidates only shrinks the set: a worklist reaches the
	 * same greatest fixed point in any order. */
	std::vector<uint32_t> pending;
	for (uint32_t value_index = plan->value_count; value_index-- > 0;) {
		if (scalar_copy_phi_candidates[value_index] != 0) {
			pending.push_back(value_index);
		}
	}
	while (!pending.empty()) {
		const uint32_t value_index = pending.back();
		pending.pop_back();
		if (scalar_copy_phi_candidates[value_index] == 0) {
			continue;
		}
		const zend_tpde_value &result = plan->values[value_index];
		const uint32_t definition = static_cast<uint32_t>(
			plan->value_definition_instructions[value_index]);
		const zend_tpde_instruction &instruction =
			plan->instructions[definition];
		bool candidate = true;
		for (uint32_t operand = 0;
				candidate && operand < instruction.operand_count; ++operand) {
			const int32_t input_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(
					plan, &instruction, operand));
			if (input_index < 0) {
				candidate = false;
				break;
			}
			const zend_tpde_value &input =
				plan->values[static_cast<uint32_t>(input_index)];
			candidate = scalar_machine_definition(input)
				|| (input.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& !input.canonical_alias_observable
					&& input.canonical_storage_id
						== result.canonical_storage_id
					&& scalar_copy_phi_candidates[
						static_cast<uint32_t>(input_index)] != 0);
		}
		if (candidate) {
			continue;
		}
		scalar_copy_phi_candidates[value_index] = 0;
		visit_operand_users(plan, value_index, [&](uint32_t user) {
			const int32_t user_result = zend_tpde_value_index(plan,
				zend_tpde_instruction_record_at(
					plan, &plan->instructions[user]).result_id);
			if (user_result >= 0
					&& scalar_copy_phi_candidates[
						static_cast<uint32_t>(user_result)] != 0) {
				pending.push_back(static_cast<uint32_t>(user_result));
			}
		});
	}

	sweep_to_fixed_point(plan->instruction_count,
		[&](uint32_t index) -> int32_t {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if ((record.opcode != ZEND_MIR_OPCODE_COPY
					&& record.opcode != ZEND_MIR_OPCODE_PHI)
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count == 0) {
				return -1;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (result_index < 0) {
				return -1;
			}
			zend_tpde_value &result =
				plan->values[static_cast<uint32_t>(result_index)];
			if (result.constant || result.canonical_alias_observable
					|| result.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL) {
				return -1;
			}

			zend_mir_scalar_type_mask input_type =
				ZEND_MIR_SCALAR_TYPE_NONE;
			zend_mir_representation input_representation =
				ZEND_MIR_REPRESENTATION_VOID;
			zend_tpde_machine_value_kind input_kind =
				ZEND_TPDE_MACHINE_VALUE_I64;
			bool compatible = true;
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const int32_t input_index = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, operand));
				if (input_index < 0) {
					compatible = false;
					break;
				}
				const zend_tpde_value &input =
					plan->values[static_cast<uint32_t>(input_index)];
				const bool machine_definition =
					scalar_machine_definition(input);
				if (!machine_definition) {
					/*
					 * In a cyclic loop-carried ZVAL component an incoming
					 * COPY/PHI candidate of the same slot may not be selected
					 * yet: defer that edge. Other boxed inputs are a boundary.
					 */
					if (input.representation == ZEND_MIR_REPRESENTATION_ZVAL
							&& !input.canonical_alias_observable
							&& input.canonical_storage_id
								== result.canonical_storage_id
							&& scalar_copy_phi_candidates[
								static_cast<uint32_t>(input_index)] != 0) {
						continue;
					}
					compatible = false;
					break;
				}
				if (!zend_mir_scalar_type_is_exact(input.exact_type)
						|| input.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
						|| (input_type != ZEND_MIR_SCALAR_TYPE_NONE
							&& (input.exact_type != input_type
								|| input.representation
									!= input_representation
								|| input.machine_kind != input_kind))) {
					compatible = false;
					break;
				}
				if (input_type == ZEND_MIR_SCALAR_TYPE_NONE) {
					input_type = input.exact_type;
					input_representation = input.representation;
					input_kind = input.machine_kind;
				}
			}
			if (!compatible
					|| input_type == ZEND_MIR_SCALAR_TYPE_NONE
					|| input_representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				return -1;
			}
			result.representation = input_representation;
			result.exact_type = input_type;
			result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
			result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
			zend_tpde_apply_machine_value_facts(&result, true);
			return result_index;
		},
		[&](uint32_t value_index, const auto &mark) {
			visit_operand_users(plan, value_index, mark);
		});
}

static void freeze_machine_register_authority(
		zend_tpde_plan *plan) {
	freeze_machine_scalar_definitions(plan);
	for (uint32_t index = 0; index < plan->value_count; ++index) {
		zend_tpde_value &value = plan->values[index];
		const int32_t definition =
			plan->value_definition_instructions != nullptr
				? plan->value_definition_instructions[index] : -1;
		const zend_mir_instruction_record definition_record =
			definition >= 0
					&& static_cast<uint32_t>(definition)
						< plan->instruction_count
				? zend_tpde_instruction_record_at(
					plan, &plan->instructions[
						static_cast<uint32_t>(definition)])
				: zend_mir_instruction_record{};
		const bool boxed_join =
			value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
			&& (definition_record.opcode == ZEND_MIR_OPCODE_COPY
				|| definition_record.opcode == ZEND_MIR_OPCODE_PHI);
		const bool argument_abi =
			value.argument_index >= 0 && value.local_abi.valid;
		const zend_mir_scalar_type_mask exact_type =
			argument_abi ? value.local_abi.exact_type : value.exact_type;
		const zend_tpde_machine_value_kind machine_kind =
			argument_abi ? value.local_abi.machine_kind : value.machine_kind;
		value.register_authoritative =
			!value.constant
			&& !boxed_join
			&& exact_type != ZEND_MIR_SCALAR_TYPE_NULL
			&& (argument_abi
				|| value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER)
			&& (argument_abi
				|| zend_mir_scalar_type_is_exact(exact_type)
				|| machine_value_kind_can_be_register_authoritative(
					machine_kind));
	}

	for (uint32_t index = 0;
			index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const int32_t mutation =
			machine_plan_guarded_mutation_value_index(
				plan, instruction);
		if (mutation >= 0) {
			zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(mutation)];
			if ((zend_mir_scalar_type_is_exact(value.exact_type)
						&& value.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| machine_value_kind_can_be_register_authoritative(
						value.machine_kind)) {
				value.register_authoritative = true;
			}
		}

		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const int32_t result =
				zend_tpde_value_index(plan, record.result_id);
			if (result >= 0) {
				zend_tpde_value &value = plan->values[result];
				if (value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| (zend_mir_scalar_type_is_exact(
								value.exact_type)
							&& value.exact_type
								!= ZEND_MIR_SCALAR_TYPE_NULL)) {
					value.register_authoritative = true;
				}
			}
		}
	}

	sweep_to_fixed_point(plan->instruction_count,
		[&](uint32_t index) -> int32_t {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if ((record.opcode != ZEND_MIR_OPCODE_COPY
						&& record.opcode != ZEND_MIR_OPCODE_PHI)
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count == 0) {
				return -1;
			}
			const int32_t result =
				zend_tpde_value_index(plan, record.result_id);
			if (result < 0
					|| plan->values[result].machine_kind
						!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					|| plan->values[result].register_authoritative) {
				return -1;
			}
			bool all_inputs_register = true;
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const int32_t input = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, operand));
				if (input < 0
						|| plan->values[input].machine_kind
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| !plan->values[input]
							.register_authoritative) {
					all_inputs_register = false;
					break;
				}
			}
			if (!all_inputs_register) {
				return -1;
			}
			plan->values[result].register_authoritative = true;
			return result;
		},
		[&](uint32_t value_index, const auto &mark) {
			visit_operand_users(plan, value_index, mark);
		});
}

static bool machine_plan_local_abi_call_eligible(
		const zend_tpde_plan *plan, uint32_t instruction_index) {
	if (instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(
			plan, &plan->instructions[instruction_index]);
	return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& ((plan->typed_component_call_eligible != nullptr
				&& plan->typed_component_call_eligible[instruction_index] != 0)
			|| (plan->effect_closed_inline_eligible != nullptr
				&& plan->effect_closed_inline_eligible[instruction_index] != 0));
}

/* A direct user call whose frame the caller builds inline. */
static bool machine_plan_inline_frame_call(
		const zend_tpde_instruction &instruction) {
	return instruction.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& instruction.direct_call != nullptr
		&& (instruction.direct_call->flags
			& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
}

static bool machine_plan_call_arguments_require_values(
		const zend_tpde_plan *plan,
		uint32_t instruction_index) {
	if (machine_plan_local_abi_call_eligible(plan, instruction_index)) {
		return true;
	}
	if (instruction_index >= plan->instruction_count) {
		return false;
	}
	return machine_plan_inline_frame_call(
		plan->instructions[instruction_index]);
}

/* Register authority, eligibility and the register-result flags are final
 * once the machine operand transports are frozen. */
static void freeze_register_cond_branch_modes(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		instruction.register_cond_branch_modes =
			(machine_cfg_register_cond_branch(plan, instruction, false)
				? ZEND_TPDE_MACHINE_MODE_ENTRY : 0)
			| (machine_cfg_register_cond_branch(plan, instruction, true)
				? ZEND_TPDE_MACHINE_MODE_TYPED_BODY : 0);
	}
}

static bool register_cond_branch(
		const zend_tpde_instruction &instruction, bool typed_body) {
	return (instruction.register_cond_branch_modes
		& zend_tpde_machine_mode(typed_body)) != 0;
}

static bool machine_plan_use_requires_value(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use,
		bool typed_body) {
	if (use.instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &instruction =
		plan->instructions[use.instruction_index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &instruction);
	switch (use.kind) {
		case ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION:
		case ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE:
			return true;
		case ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT:
			return machine_plan_local_abi_call_eligible(
				plan, use.instruction_index);
		case ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT:
			return machine_plan_inline_frame_call(instruction);
		case ZEND_TPDE_MACHINE_USE_PHI_EDGE:
			return machine_plan_value_needs_result_assignment(
				plan, record.result_id);
		case ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND:
			return false;
		case ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND:
			break;
	}

	const bool machine_result =
		machine_plan_value_needs_result_assignment(
			plan, record.result_id);
	if (record.opcode == ZEND_MIR_OPCODE_COPY
			&& record.representation == ZEND_MIR_REPRESENTATION_ZVAL) {
		return machine_result && use.operand_index == 0;
	}
	if (!machine_result
			&& (record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
				|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64)) {
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
		return use.operand_index == 0;
	}
	if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
			&& (typed_body || plan->has_register_component_results)) {
		return use.operand_index == 0;
	}
	const bool boxed_cond_branch =
		(instruction.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) != 0
		&& !register_cond_branch(instruction, typed_body);
	if (boxed_cond_branch
			|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& !typed_body && !plan->has_register_component_results)
			|| record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
			|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr)) {
		return false;
	}
	return use.operand_index < instruction.operand_count;
}

static uint8_t machine_plan_use_value_modes(
		const zend_tpde_plan *plan, const zend_tpde_machine_use &use) {
	return (machine_plan_use_requires_value(plan, use, false)
			? ZEND_TPDE_MACHINE_MODE_ENTRY : 0)
		| (machine_plan_use_requires_value(plan, use, true)
			? ZEND_TPDE_MACHINE_MODE_TYPED_BODY : 0);
}

/*
 * operand_value_modes receives, per instruction operand, the modes whose
 * machine function needs the operand's value (machine_plan_use_value_modes);
 * freeze_machine_required_values reads it for the same uses.
 */
[[nodiscard]]
static bool freeze_machine_operand_transports(
		zend_tpde_plan *plan, std::vector<uint8_t> &operand_value_modes,
		zend_native_diagnostic *diag) {
	operand_value_modes.assign(plan->instruction_operand_count, 0);
	if (plan->instruction_operand_count == 0) {
		return true;
	}
	if (!plan_alloc(plan->instruction_operand_transports,
			plan->instruction_operand_count, diag,
			"unable to freeze machine operand transports")) {
		return false;
	}

	/*
	 * Transient definitions first: a scalar stored by ZVAL_STORE is not
	 * register-authoritative, but the store consumes the computed payload
	 * rather than reloading the destination zval.
	 */
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &store =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &store);
		if (record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE
				|| store.operand_count == 0) {
			continue;
		}
		const int32_t value_index = zend_tpde_value_index(
			plan, zend_tpde_operand_at(plan, &store, 0));
		if (value_index < 0) {
			continue;
		}
		const zend_tpde_value &value =
			plan->values[static_cast<uint32_t>(value_index)];
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (value.constant || value.register_authoritative
				|| !zend_mir_scalar_type_is_exact(value.exact_type)
				|| value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
				|| definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &defining =
			plan->instructions[static_cast<uint32_t>(definition)];
		defining.transient_scalar_result = true;
		defining.transient_result_representation =
			zend_tpde_scalar_representation(value.exact_type);
		defining.transient_result_exact_type = value.exact_type;
		defining.transient_result_machine_kind =
			zend_tpde_scalar_machine_kind(value.exact_type);
		defining.transient_result_storage_id = value.canonical_storage_id;
	}

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		for (uint32_t operand_index = 0;
				operand_index < instruction.operand_count;
				++operand_index) {
			const zend_tpde_machine_use use{
				instruction_index,
				operand_index,
				record.opcode == ZEND_MIR_OPCODE_PHI
					? operand_index : UINT32_MAX,
				record.opcode == ZEND_MIR_OPCODE_PHI
					? ZEND_TPDE_MACHINE_USE_PHI_EDGE
					: ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND,
			};
			const uint8_t modes = machine_plan_use_value_modes(plan, use);
			operand_value_modes[instruction.operand_offset + operand_index] =
				modes;
			if (modes == 0) {
				continue;
			}
			const int32_t value_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(
					plan, &instruction, operand_index));
			if (value_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine operand transport references an unknown value");
				return false;
			}
			const zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(value_index)];
			zend_mir_scalar_type_mask transport_type = value.exact_type;
			zend_mir_representation transport_representation =
				value.representation;
			const zend_mir_scalar_descriptor *scalar_descriptor =
				zend_mir_scalar_descriptor_at(record.opcode);
			if (!zend_mir_scalar_type_is_exact(transport_type)
					&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& scalar_descriptor != nullptr
					&& operand_index < scalar_descriptor->operand_count
					&& zend_mir_scalar_type_is_exact(
						scalar_descriptor->operands[operand_index].exact_type)) {
				/*
				 * Scalar lowering may consume a canonical ZVAL value on a
				 * stronger source proof than the merged MIR fact: load the
				 * payload the descriptor requires, so the register allocator
				 * never sees an undefined ZVAL machine value.
				 */
				transport_type =
					scalar_descriptor->operands[operand_index].exact_type;
				transport_representation =
					scalar_descriptor->operands[operand_index].representation;
			}
			const int32_t definition =
				plan->value_definition_instructions == nullptr
					? -1 : plan->value_definition_instructions[value_index];
			const bool transient_store_use =
				record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& operand_index == 0
				&& definition >= 0
				&& static_cast<uint32_t>(definition)
					< plan->instruction_count
				&& plan->instructions[static_cast<uint32_t>(definition)]
					.transient_scalar_result;
			if (value.constant || value.register_authoritative
					|| transient_store_use
					|| transport_type == ZEND_MIR_SCALAR_TYPE_NULL
					|| !zend_mir_scalar_type_is_exact(transport_type)) {
				continue;
			}
			if (!zend_mir_id_is_valid(value.canonical_storage_id)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"slot-authoritative scalar has no canonical storage");
				return false;
			}
			zend_tpde_operand_transport &transport =
				plan->instruction_operand_transports[
					instruction.operand_offset + operand_index];
			transport = {
				ZEND_TPDE_OPERAND_TRANSPORT_CANONICAL_SCALAR_LOAD,
				transport_representation,
				transport_type,
				zend_tpde_scalar_machine_kind(transport_type),
				value.canonical_storage_id,
				value.canonical_alias_observable
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR,
			};
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_machine_required_values(
		zend_tpde_plan *plan,
		const std::vector<uint8_t> &operand_value_modes,
		zend_native_diagnostic *diag) {
	if (plan->value_count == 0) {
		return true;
	}
	plan->entry_value_required = static_cast<uint8_t *>(
		std::calloc(plan->value_count, sizeof(uint8_t)));
	plan->typed_body_value_required = static_cast<uint8_t *>(
		std::calloc(plan->value_count, sizeof(uint8_t)));
	if (plan->entry_value_required == nullptr
			|| plan->typed_body_value_required == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze machine-plan value uses");
		return false;
	}

	if (plan->call_argument_bindings != nullptr) {
		for (uint32_t instruction_index = 0;
				instruction_index < plan->instruction_count;
				++instruction_index) {
			if (!machine_plan_call_arguments_require_values(
					plan, instruction_index)) {
				continue;
			}
			const zend_tpde_instruction &instruction =
				plan->instructions[instruction_index];
			if (instruction.call_argument_offset > plan->call_argument_count
					|| instruction.call_argument_count
						> plan->call_argument_count
							- instruction.call_argument_offset) {
				continue;
			}
			for (uint32_t argument = 0;
					argument < instruction.call_argument_count;
					++argument) {
				const uint32_t argument_index =
					instruction.call_argument_offset + argument;
				const int32_t value_index =
					plan->call_argument_bindings[argument_index].value_index;
				if (value_index < 0
						|| static_cast<uint32_t>(value_index)
							>= plan->value_count) {
					continue;
				}
				plan->entry_value_required[value_index] = 1;
				plan->typed_body_value_required[value_index] = 1;
			}
		}
	}
	if (plan->value_consumer_offsets != nullptr) {
		for (uint32_t value = 0; value < plan->value_count; ++value) {
			const uint32_t begin = plan->value_consumer_offsets[value];
			const uint32_t end = plan->value_consumer_offsets[value + 1];
			for (uint32_t use = begin; use < end; ++use) {
				const zend_tpde_machine_use &consumer =
					plan->value_consumers[use];
				const uint8_t modes =
					consumer.kind == ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND
						|| consumer.kind == ZEND_TPDE_MACHINE_USE_PHI_EDGE
					? operand_value_modes[
						plan->instructions[consumer.instruction_index]
							.operand_offset + consumer.operand_index]
					: machine_plan_use_value_modes(plan, consumer);
				plan->entry_value_required[value] |=
					(modes & ZEND_TPDE_MACHINE_MODE_ENTRY) != 0;
				plan->typed_body_value_required[value] |=
					(modes & ZEND_TPDE_MACHINE_MODE_TYPED_BODY) != 0;
			}
		}
	}

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const int32_t value_index =
			machine_cfg_register_cond_branch_value_index(
				plan, instruction);
		if (value_index < 0
				|| static_cast<uint32_t>(value_index)
					>= plan->value_count) {
			continue;
		}
		const uint32_t value = static_cast<uint32_t>(value_index);
		if (register_cond_branch(instruction, false)) {
			plan->entry_value_required[value] = 1;
		}
		if (register_cond_branch(instruction, true)) {
			plan->typed_body_value_required[value] = 1;
		}
	}

	/* A typed body returns the value bound to RETURN_SOURCE_ZVAL, which is
	 * no MIR operand of it; that value must be defined in the typed body. */
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const int32_t value_index =
			instruction.source_op1_binding.value_index;
		if (zend_tpde_instruction_record_at(plan, &instruction).opcode
					== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& value_index >= 0
				&& static_cast<uint32_t>(value_index) < plan->value_count) {
			plan->typed_body_value_required[value_index] = 1;
		}
	}

	/* A required TPDE value needs a machine representation; canonical-slot
	 * values serve source helpers but take no part in register liveness. */
	for (uint32_t value = 0; value < plan->value_count; ++value) {
		if (machine_plan_value_has_result_representation(
				plan, plan->values[value].id)) {
			continue;
		}
		plan->entry_value_required[value] = 0;
		plan->typed_body_value_required[value] = 0;
	}

	return true;
}

[[nodiscard]]
static bool freeze_machine_cfg(
		zend_tpde_plan *plan,
		bool typed_body,
		zend_tpde_machine_cfg *cfg,
		zend_native_diagnostic *diag) {
	std::vector<uint32_t> instruction_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_cold_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_hot_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_continuation_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> boxed_cond_cold_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> boxed_cond_cold_by_predecessor(
		plan->block_count, UINT32_MAX);
	std::vector<uint32_t> final_blocks(plan->block_count);
	/* The MIR block of each instruction. */
	std::vector<uint32_t> source_blocks(plan->instruction_count);
	uint32_t synthetic_block_count = 0;

	for (uint32_t block = 0; block < plan->block_count; ++block) {
		final_blocks[block] = block;
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const int32_t source_block =
			zend_tpde_block_index(plan, record.block_id);
		if (source_block < 0) {
			goto malformed;
		}
		const uint32_t source = static_cast<uint32_t>(source_block);
		source_blocks[index] = source;
		instruction_blocks[index] = final_blocks[source];
		const bool typed_component_call =
			(instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_TYPED_COMPONENT_CALL) != 0
			&& plan->typed_component_call_eligible != nullptr
			&& plan->typed_component_call_eligible[index] != 0;
		/* A typed body emits proven operations without a cold edge. */
		const bool guarded =
			typed_component_call
				? !typed_body
				: (instruction.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) != 0
					&& !(!typed_body && instruction.fused_into_branch)
					&& !(typed_body
						&& (zend_tpde_typed_numeric_binary(plan, instruction)
							|| zend_tpde_typed_forward_assign(instruction)));
		if (!guarded) {
			continue;
		}
		if (typed_component_call && !typed_body) {
			guarded_hot_blocks[index] =
				plan->block_count + synthetic_block_count++;
		}
		guarded_cold_blocks[index] =
			plan->block_count + synthetic_block_count++;
		guarded_continuation_blocks[index] =
			plan->block_count + synthetic_block_count++;
		final_blocks[source] = guarded_continuation_blocks[index];
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		zend_tpde_value_condition condition{};
		const bool boxed_branch =
			(instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) != 0
			&& !register_cond_branch(instruction, typed_body)
			&& zend_tpde_value_condition_at(instruction, &condition);
		if (!boxed_branch) {
			continue;
		}
		const uint32_t predecessor = instruction_blocks[index];
		if (predecessor == UINT32_MAX
				|| predecessor >= plan->block_count
					+ synthetic_block_count) {
			goto malformed;
		}
		const uint32_t cold_block =
			plan->block_count + synthetic_block_count++;
		boxed_cond_cold_blocks[index] = cold_block;
		boxed_cond_cold_by_predecessor[source_blocks[index]] = cold_block;
	}

	{
		const uint32_t block_count =
			plan->block_count + synthetic_block_count;
		/* The edges in insertion order; each block keeps the order of its
		 * own edges. */
		std::vector<std::pair<uint32_t, uint32_t>> edges;
		std::vector<uint32_t> finally_return_blocks;
		std::vector<uint32_t> finally_targets;
		auto add_edge = [&](uint32_t from, uint32_t to) {
			if (from >= block_count || to >= block_count) {
				return false;
			}
			edges.emplace_back(from, to);
			return true;
		};

		for (uint32_t block = 0; block < plan->block_count; ++block) {
			for (uint32_t edge = plan->block_successor_offsets[block];
					edge < plan->block_successor_offsets[block + 1];
					++edge) {
				if (!add_edge(
						final_blocks[block],
						plan->block_successors[edge])) {
					goto malformed;
				}
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const uint32_t cold = guarded_cold_blocks[index];
			if (cold == UINT32_MAX) {
				continue;
			}
			const uint32_t hot = guarded_hot_blocks[index];
			const uint32_t continuation =
				guarded_continuation_blocks[index];
			if (!add_edge(
					instruction_blocks[index],
					hot == UINT32_MAX ? continuation : hot)
					|| !add_edge(instruction_blocks[index], cold)
					|| (hot != UINT32_MAX
						&& !add_edge(hot, continuation))
					|| (hot != UINT32_MAX
						&& plan->instructions[index].typed_call_may_fail
						&& !add_edge(hot, cold))
					/* A deoptimization exit leaves the function, but keeps
					 * the edge: TPDE then compiles its block right after the
					 * fast path, whose guards branch there with the fast
					 * path's register assignment. */
					|| !add_edge(cold, continuation)) {
				goto malformed;
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const uint32_t cold = boxed_cond_cold_blocks[index];
			if (cold == UINT32_MAX) {
				continue;
			}
			const uint32_t source_predecessor = source_blocks[index];
			if (plan->block_successor_offsets[
							source_predecessor + 1]
						- plan->block_successor_offsets[
							source_predecessor]
						!= 2
					|| !add_edge(instruction_blocks[index], cold)) {
				goto malformed;
			}
			for (uint32_t edge =
						plan->block_successor_offsets[source_predecessor];
					edge < plan->block_successor_offsets[
						source_predecessor + 1];
					++edge) {
				if (!add_edge(cold, plan->block_successors[edge])) {
					goto malformed;
				}
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			const uint32_t record_block = instruction_blocks[index];
			if (record_block == UINT32_MAX) {
				goto malformed;
			}
			if (zend_mir_id_is_valid(instruction.exception_block_id)) {
				const int32_t exception_block = zend_tpde_block_index(
					plan, instruction.exception_block_id);
				if (exception_block < 0
						|| !add_edge(
							guarded_cold_blocks[index] == UINT32_MAX
								? record_block
								: guarded_cold_blocks[index],
							static_cast<uint32_t>(exception_block))) {
					goto malformed;
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_FINALLY_RETURN) {
				finally_return_blocks.push_back(record_block);
			} else if (record.opcode == ZEND_MIR_OPCODE_FINALLY_CALL) {
				const uint32_t source_block = source_blocks[index];
				if (plan->block_successor_offsets[source_block + 1]
							- plan->block_successor_offsets[source_block]
							!= 2) {
					goto malformed;
				}
				finally_targets.push_back(
					plan->block_successors[
						plan->block_successor_offsets[source_block] + 1]);
			} else if ((record.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
						|| record.opcode == ZEND_MIR_OPCODE_FINALLY_ENTER)
					&& record.block_id
						!= plan->function.entry_block_id) {
				finally_targets.push_back(record_block);
			}
		}
		for (uint32_t return_block : finally_return_blocks) {
			for (uint32_t target : finally_targets) {
				if (!add_edge(return_block, target)) {
					goto malformed;
				}
			}
		}

		/* Group the edges by block (a stable counting sort), then drop
		 * repeated targets except an identical two-way branch. */
		std::vector<uint32_t> edge_offsets(block_count + 1, 0);
		for (const auto &[from, to] : edges) {
			++edge_offsets[from + 1];
		}
		for (uint32_t block = 0; block < block_count; ++block) {
			edge_offsets[block + 1] += edge_offsets[block];
		}
		std::vector<uint32_t> edge_targets(edges.size());
		{
			std::vector<uint32_t> next = edge_offsets;
			for (const auto &[from, to] : edges) {
				edge_targets[next[from]++] = to;
			}
		}
		std::vector<uint32_t> successor_offsets(block_count + 1);
		std::vector<uint32_t> successors;
		std::vector<uint32_t> seen(block_count, UINT32_MAX);
		for (uint32_t block = 0; block < block_count; ++block) {
			const std::span<const uint32_t> block_successors{
				edge_targets.data() + edge_offsets[block],
				edge_offsets[block + 1] - edge_offsets[block]};
			const bool identical_branch_targets =
				block_successors.size() == 2
				&& block_successors[0] == block_successors[1];
			for (uint32_t target : block_successors) {
				if (!identical_branch_targets && seen[target] == block) {
					continue;
				}
				seen[target] = block;
				successors.push_back(target);
			}
			if (successors.size() > MAX_RECORDS) {
				goto malformed;
			}
			successor_offsets[block + 1] =
				static_cast<uint32_t>(successors.size());
		}
		cfg->block_count = block_count;
		cfg->successor_count =
			static_cast<uint32_t>(successors.size());
		if (!freeze_machine_cfg_array(
					&cfg->successor_offsets, successor_offsets)
				|| !freeze_machine_cfg_array(
					&cfg->successors, successors)
				|| !freeze_machine_cfg_array(
					&cfg->instruction_blocks, instruction_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_cold_blocks, guarded_cold_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_hot_blocks, guarded_hot_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_continuation_blocks,
					guarded_continuation_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->final_blocks, final_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->boxed_cond_cold_blocks,
					boxed_cond_cold_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->boxed_cond_cold_by_predecessor,
					boxed_cond_cold_by_predecessor)) {
			destroy_machine_cfg(cfg);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to allocate frozen TPDE control-flow plan");
			return false;
		}
	}
	return true;

malformed:
	destroy_machine_cfg(cfg);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"unable to freeze TPDE control-flow plan");
	return false;
}

/*
 * A member returns a number when each of its returns yields a long or a
 * double. Results of calls to such members are numbers too, so arithmetic on
 * them cannot fail. Start from all members and drop those with another
 * return until nothing changes; by induction on the call depth every result
 * of a remaining member is a number. A call whose checked arguments are all
 * numbers targets the callee's numeric variant; a call that loses this proof
 * keeps the general target, so both sets only shrink.
 */
static void freeze_component_numeric_operands(
		zend_tpde_plan *plans, uint32_t component_count) {
	std::vector<uint8_t> returns_number(component_count, 1);
	for (uint32_t member = 0; member < component_count; ++member) {
		const zend_tpde_plan &plan = plans[member];
		if (!zend_tpde_linux_inline_forms || plan.generator_resume_count != 0
				|| plan.source_opcodes == nullptr
				|| plan.return_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
			returns_number[member] = 0;
		}
	}
	auto numeric_binary = [](const zend_tpde_plan &plan,
			const zend_tpde_instruction &instruction) {
		const uint32_t opcode = instruction.value_operation.source_opcode;
		return instruction.has_value_operation
			&& instruction.value_operation.opcode
				== ZEND_MIR_OPCODE_VALUE_BINARY_OP
			&& (opcode == ZEND_ADD || opcode == ZEND_SUB
				|| opcode == ZEND_MUL || opcode == ZEND_IS_SMALLER
				|| opcode == ZEND_IS_SMALLER_OR_EQUAL
				|| opcode == ZEND_IS_EQUAL || opcode == ZEND_IS_NOT_EQUAL)
			&& instruction.value_operation.source_position_id
				< plan.source_opcode_count
			&& plan.source_opcodes[
				instruction.value_operation.source_position_id].opcode
				== opcode;
	};
	auto number_value = [&](const zend_tpde_plan &plan,
			const zend_tpde_source_value_binding &binding, uint8_t known) {
		if (zend_tpde_known_numeric_type(known)) {
			return true;
		}
		if (binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan.value_count
				&& (plan.values[binding.value_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_I64
					|| plan.values[binding.value_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_F64)) {
			return true;
		}
		int32_t definition = binding.definition_instruction_index;
		if (definition < 0 && binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan.value_count
				&& plan.source_value_definition_instructions != nullptr) {
			definition = plan.source_value_definition_instructions[
				binding.value_index];
		}
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan.instruction_count) {
			return false;
		}
		const zend_tpde_instruction &producer =
			plan.instructions[definition];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(&plan, &producer);
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			return producer.direct_call != nullptr
				&& producer.component_target_index < component_count
				&& returns_number[producer.component_target_index] != 0;
		}
		return producer.numeric_operands_proven
			&& (producer.value_operation.source_opcode == ZEND_ADD
				|| producer.value_operation.source_opcode == ZEND_SUB
				|| producer.value_operation.source_opcode == ZEND_MUL);
	};
	for (uint32_t member = 0; member < component_count; ++member) {
		zend_tpde_plan &plan = plans[member];
		for (uint32_t index = 0; index < plan.instruction_count; ++index) {
			zend_tpde_instruction &call = plan.instructions[index];
			if (call.numeric_variant_plus_one != 0) {
				call.component_target_index = zend_tpde_linux_inline_forms
						&& call.numeric_variant_plus_one - 1 < component_count
					? call.numeric_variant_plus_one - 1
					: call.numeric_variant_general;
			}
		}
	}
	bool changed;
	do {
		changed = false;
		for (uint32_t member = 0; member < component_count; ++member) {
			zend_tpde_plan &plan = plans[member];
			if (!zend_tpde_linux_inline_forms || plan.source_opcodes == nullptr) {
				continue;
			}
			for (uint32_t index = 0; index < plan.instruction_count; ++index) {
				zend_tpde_instruction &call = plan.instructions[index];
				if (call.numeric_variant_plus_one == 0
						|| call.component_target_index
							== call.numeric_variant_general) {
					continue;
				}
				bool numbers = plan.call_argument_bindings != nullptr;
				for (uint32_t argument = 0; numbers
						&& argument < call.call_argument_count; ++argument) {
					zend_mir_call_argument_ref source_argument{};
					const uint32_t argument_index =
						call.call_argument_offset + argument;
					if (!zend_tpde_call_argument_at(
							&plan, argument_index, &source_argument)) {
						numbers = false;
						break;
					}
					if (source_argument.ordinal >= 32
							|| ((call.numeric_variant_mask
								>> source_argument.ordinal) & 1) == 0) {
						continue;
					}
					numbers = source_argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
						&& source_argument.send_opline_index
							< plan.source_opcode_count
						&& number_value(plan,
							plan.call_argument_bindings[argument_index],
							plan.source_opcodes[
								source_argument.send_opline_index]
								.op1_known_type);
				}
				/* Every checked argument must be present. */
				uint32_t present = 0;
				for (uint32_t argument = 0; numbers
						&& argument < call.call_argument_count; ++argument) {
					zend_mir_call_argument_ref source_argument{};
					if (zend_tpde_call_argument_at(&plan,
							call.call_argument_offset + argument,
							&source_argument)
							&& source_argument.ordinal < 32) {
						present |= UINT32_C(1) << source_argument.ordinal;
					}
				}
				if (!numbers
						|| (present & call.numeric_variant_mask)
							!= call.numeric_variant_mask) {
					call.component_target_index = call.numeric_variant_general;
					changed = true;
				}
			}
			bool saw_return = false;
			bool all_numbers = true;
			for (uint32_t index = 0; index < plan.instruction_count; ++index) {
				zend_tpde_instruction &instruction = plan.instructions[index];
				const zend_mir_instruction_record record =
					zend_tpde_instruction_record_at(&plan, &instruction);
				if (numeric_binary(plan, instruction)) {
					const zend_tpde_source_opcode &source =
						plan.source_opcodes[
							instruction.value_operation.source_position_id];
					instruction.numeric_operands_proven =
						number_value(plan, instruction.source_op1_binding,
							source.op1_known_type)
						&& number_value(plan, instruction.source_op2_binding,
							source.op2_known_type);
				}
				if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
					saw_return = true;
					const int32_t returned = instruction.operand_count == 1
						? zend_tpde_value_index(&plan,
							zend_tpde_operand_at(&plan, &instruction, 0))
						: -1;
					all_numbers = all_numbers && returned >= 0
						&& (plan.values[returned].exact_type
								== ZEND_MIR_SCALAR_TYPE_I64
							|| plan.values[returned].exact_type
								== ZEND_MIR_SCALAR_TYPE_F64);
				} else if (record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
					saw_return = true;
					const uint32_t position =
						instruction.value_operation.source_position_id;
					all_numbers = all_numbers
						&& instruction.has_value_operation
						&& instruction.value_operation.source_opcode
							== ZEND_RETURN
						&& position < plan.source_opcode_count
						&& number_value(plan, instruction.source_op1_binding,
							plan.source_opcodes[position].op1_known_type);
				} else if (record.opcode == ZEND_MIR_OPCODE_FINALLY_RETURN
						|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_RETURN) {
					all_numbers = false;
				}
			}
			const uint8_t number = returns_number[member] != 0
				&& saw_return && all_numbers;
			if (number != returns_number[member]) {
				returns_number[member] = number;
				changed = true;
			}
		}
	} while (changed);
	for (uint32_t member = 0; member < component_count; ++member) {
		plans[member].returns_number = returns_number[member] != 0;
	}
}

bool freeze_component_machine_plan(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag) {
	freeze_component_numeric_operands(plans, component_count);
	for (uint32_t component = 0;
			component < component_count; ++component) {
		freeze_effect_closed_argument_use(&plans[component]);
	}
	for (uint32_t component = 0;
			component < component_count; ++component) {
		for (uint32_t value = 0;
				value < plans[component].value_count; ++value) {
			plans[component].values[value].local_abi =
				machine_plan_value_abi(&plans[component], value);
		}
	}
	std::vector<uint8_t> candidates(component_count, 1);
	std::vector<uint8_t> has_direct_call(component_count, 0);
	for (uint32_t index = 0; index < component_count; ++index) {
		const zend_tpde_plan &plan = plans[index];
		candidates[index] = plan_receives_variadic(&plan) ? 0 : 1;
		for (uint32_t i = 0; has_direct_call[index] == 0
				&& i < plan.instruction_count; ++i) {
			has_direct_call[index] = zend_tpde_instruction_record_at(
					&plan, &plan.instructions[i]).opcode
				== ZEND_MIR_OPCODE_CALL_DIRECT_USER;
		}
	}
	/* A signature depends only on the candidate set and frozen plan facts:
	 * the pass that drops no candidate computed the final ones. */
	std::vector<zend_tpde_local_abi_type> return_types(component_count);
	std::vector<uint8_t> may_fail(component_count, 0);
	typed_body_signature_scratch signature_scratch;
	/*
	 * freeze_typed_component_calls depends only on frozen plan facts and on
	 * each callee's typed_body_eligible and typed_body_return_abi. A member's
	 * call freeze is kept with those callee facts and repeated only when they
	 * change; the control-flow flags and body indices saved before the first
	 * freeze undo it. The kept freeze is final when the facts still match.
	 */
	struct component_call_freeze {
		bool frozen = false;
		bool all_typed = false;
		std::vector<std::pair<bool, zend_tpde_local_abi_type>> callees;
		std::vector<uint8_t> flags;
		std::vector<uint32_t> body_indices;
	};
	std::vector<component_call_freeze> call_freezes(component_count);
	auto callee_facts = [&](const zend_tpde_plan &plan) {
		std::vector<std::pair<bool, zend_tpde_local_abi_type>> facts;
		for (uint32_t i = 0; i < plan.instruction_count; ++i) {
			const uint32_t target = plan.instructions[i].component_target_index;
			if (zend_tpde_instruction_record_at(
						&plan, &plan.instructions[i]).opcode
					!= ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| target >= component_count) {
				continue;
			}
			facts.emplace_back(plans[target].typed_body_eligible,
				plans[target].typed_body_return_abi);
		}
		return facts;
	};
	auto same_callee_facts = [](
			const std::vector<std::pair<bool, zend_tpde_local_abi_type>> &a,
			const std::vector<std::pair<bool, zend_tpde_local_abi_type>> &b) {
		return std::ranges::equal(a, b, [](const auto &x, const auto &y) {
			return x.first == y.first
				&& x.second.valid == y.second.valid
				&& x.second.representation == y.second.representation
				&& x.second.exact_type == y.second.exact_type
				&& x.second.machine_kind == y.second.machine_kind
				&& x.second.transfer == y.second.transfer;
		});
	};
	auto unfreeze_component_calls = [&](uint32_t index) {
		zend_tpde_plan &plan = plans[index];
		component_call_freeze &freeze = call_freezes[index];
		if (!freeze.frozen) {
			return;
		}
		std::free(plan.typed_component_call_eligible);
		std::free(plan.effect_closed_inline_eligible);
		plan.typed_component_call_eligible = nullptr;
		plan.effect_closed_inline_eligible = nullptr;
		plan.has_register_component_results = false;
		for (uint32_t i = 0; i < plan.instruction_count; ++i) {
			plan.instructions[i].machine_control_flow_flags = freeze.flags[i];
			plan.instructions[i].component_body_function_index =
				freeze.body_indices[i];
		}
		freeze.frozen = false;
	};
	bool nested_dropped;
	do {
	nested_dropped = false;
	bool changed;
	do {
		changed = false;
		for (uint32_t index = 0; index < component_count; ++index) {
			if (candidates[index] == 0) {
				continue;
			}
			zend_tpde_local_abi_type return_type{};
			bool member_may_fail = false;
			if (!try_freeze_typed_body_signature(
					&plans[index], component_plans, component_count,
					candidates.data(), signature_scratch,
					&return_type, &member_may_fail)) {
				candidates[index] = 0;
				changed = true;
			}
			return_types[index] = return_type;
			may_fail[index] = member_may_fail ? 1 : 0;
		}
	} while (changed);
	for (uint32_t index = 0; index < component_count; ++index) {
		plans[index].typed_body_eligible = candidates[index] != 0;
		plans[index].typed_body_may_fail =
			plans[index].typed_body_eligible && may_fail[index] != 0;
		plans[index].typed_body_return_abi =
			plans[index].typed_body_eligible
				? return_types[index] : zend_tpde_local_abi_type{};
	}
	/*
	 * A typed body has no frame for a phased call, so every direct user call
	 * in it must become a typed component call; nested calls feed inner
	 * results to outer arguments. Check this with a dry run of the call
	 * freeze, and give up the typed body otherwise; callers of that member
	 * then have to be decided again.
	 */
	for (uint32_t index = 0; index < component_count; ++index) {
		zend_tpde_plan &plan = plans[index];
		if (!plan.typed_body_eligible || has_direct_call[index] == 0) {
			continue;
		}
		component_call_freeze &freeze = call_freezes[index];
		std::vector<std::pair<bool, zend_tpde_local_abi_type>> callees =
			callee_facts(plan);
		if (!freeze.frozen || !same_callee_facts(freeze.callees, callees)) {
			unfreeze_component_calls(index);
			if (freeze.flags.empty()) {
				freeze.flags.resize(plan.instruction_count);
				freeze.body_indices.resize(plan.instruction_count);
				for (uint32_t i = 0; i < plan.instruction_count; ++i) {
					freeze.flags[i] =
						plan.instructions[i].machine_control_flow_flags;
					freeze.body_indices[i] =
						plan.instructions[i].component_body_function_index;
				}
			}
			bool all_typed = freeze_typed_component_calls(
				&plan, component_plans, component_count);
			freeze.frozen = true;
			/* Only a Zend entry clones an effect-closed callee; a typed
			 * body needs a typed call. */
			for (uint32_t i = 0; all_typed && i < plan.instruction_count;
					++i) {
				all_typed = zend_tpde_instruction_record_at(
						&plan, &plan.instructions[i]).opcode
							!= ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| (plan.typed_component_call_eligible != nullptr
						&& plan.typed_component_call_eligible[i] != 0
						&& (plan.effect_closed_inline_eligible == nullptr
							|| plan.effect_closed_inline_eligible[i] == 0));
			}
			freeze.all_typed = all_typed;
			freeze.callees = std::move(callees);
		}
		if (!freeze.all_typed) {
			candidates[index] = 0;
			nested_dropped = true;
		}
	}
	} while (nested_dropped);
	/* A typed body that makes a typed call to a body that may fail may fail
	 * too; all its direct calls are typed. */
	for (bool grew = true; grew;) {
		grew = false;
		for (uint32_t index = 0; index < component_count; ++index) {
			zend_tpde_plan &plan = plans[index];
			if (!plan.typed_body_eligible || plan.typed_body_may_fail) {
				continue;
			}
			for (uint32_t i = 0; i < plan.instruction_count; ++i) {
				const uint32_t target =
					plan.instructions[i].component_target_index;
				if (zend_tpde_instruction_record_at(
							&plan, &plan.instructions[i]).opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& target < component_count
						&& plans[target].typed_body_eligible
						&& plans[target].typed_body_may_fail) {
					plan.typed_body_may_fail = true;
					grew = true;
					break;
				}
			}
		}
	}
	uint32_t next_typed_body_function = component_count;
	for (uint32_t index = 0; index < component_count; ++index) {
		plans[index].wrapper_function_index = index;
		plans[index].typed_body_function_index =
			plans[index].typed_body_eligible
				? next_typed_body_function++ : UINT32_MAX;
	}
	/* A native call entry leaves through the shared leave and hands any
	 * other frame to the general fast Do. */
	for (uint32_t index = 0; index < component_count; ++index) {
		if (plans[index].fast_call_eligible) {
			require_runtime_helper(
				&plans[index], ZEND_NATIVE_HELPER_CALL_FAST_DO);
			if (plans[index].fast_call_variadic) {
				require_runtime_helper(&plans[index],
					ZEND_NATIVE_HELPER_CALL_RECEIVE_VARIADIC);
			}
			require_runtime_helper(
				&plans[index], ZEND_NATIVE_HELPER_CALL_FAST_LEAVE);
			require_runtime_helper(
				&plans[index], ZEND_NATIVE_HELPER_CALL_FAST_RELEASE_CV);
			require_runtime_helper(
				&plans[index], ZEND_NATIVE_HELPER_CALL_FAST_RELEASE_THIS);
			require_runtime_helper(
				&plans[index], ZEND_NATIVE_HELPER_CALL_FAST_RUN_TIME_CACHE);
		}
	}
	for (uint32_t index = 0; index < component_count; ++index) {
		if (call_freezes[index].frozen
				&& same_callee_facts(call_freezes[index].callees,
					callee_facts(plans[index]))) {
			/* The kept freeze ran before the typed bodies were numbered. */
			zend_tpde_plan &plan = plans[index];
			for (uint32_t i = 0; i < plan.instruction_count; ++i) {
				const uint32_t target =
					plan.instructions[i].component_target_index;
				if (plan.typed_component_call_eligible != nullptr
						&& plan.typed_component_call_eligible[i] != 0
						&& target < component_count) {
					plan.instructions[i].component_body_function_index =
						plans[target].typed_body_function_index;
				}
			}
		} else {
			unfreeze_component_calls(index);
			if (!freeze_typed_component_calls(
					&plans[index], component_plans, component_count)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
					"unable to freeze component-local TPDE call plan");
				return false;
			}
		}
		bool typed_body_may_emit_calls = false;
		for (uint32_t instruction = 0;
				instruction < plans[index].instruction_count; ++instruction) {
			typed_body_may_emit_calls = typed_body_may_emit_calls
				|| (plans[index].typed_component_call_eligible != nullptr
					&& plans[index].typed_component_call_eligible[instruction] != 0);
		}
		/*
		 * Typed-body eligibility rejects effectful and helper-backed
		 * instructions, so a typed component call is the only call a typed
		 * body emits. A Zend entry keeps the helper fact and may also make a
		 * typed-body call; the unwind facts of both are kept apart.
		 */
		for (uint32_t instruction = 0;
				instruction < plans[index].instruction_count; ++instruction) {
			const uint32_t target =
				plans[index].instructions[instruction].component_target_index;
			plans[index].instructions[instruction].typed_call_may_fail =
				plans[index].typed_component_call_eligible != nullptr
				&& plans[index].typed_component_call_eligible[instruction] != 0
				&& (plans[index].effect_closed_inline_eligible == nullptr
					|| plans[index].effect_closed_inline_eligible[instruction]
						== 0)
				&& target < component_count
				&& plans[target].typed_body_eligible
				&& plans[target].typed_body_may_fail;
		}
		plans[index].typed_body_may_emit_calls =
			plans[index].typed_body_eligible && typed_body_may_emit_calls;
		plans[index].zend_entry_may_emit_calls =
			plans[index].may_emit_calls || typed_body_may_emit_calls;
		freeze_machine_register_authority(
			&plans[index]);
		demote_slot_backed_lazy_stores(&plans[index]);
		freeze_register_cond_branch_modes(&plans[index]);
		std::vector<uint8_t> operand_value_modes;
		if (!freeze_machine_operand_transports(
				&plans[index], operand_value_modes, diag)) {
			return false;
		}
		if (!retain_typed_call_materializations(&plans[index], diag)) {
			return false;
		}
		if (!freeze_machine_required_values(
				&plans[index], operand_value_modes, diag)) {
			return false;
		}
		if (!freeze_machine_cfg(
				&plans[index], false,
				&plans[index].entry_machine_cfg, diag)
				|| (plans[index].typed_body_eligible
					&& !freeze_machine_cfg(
						&plans[index], true,
						&plans[index].typed_body_machine_cfg,
						diag))) {
			return false;
		}
	}
	return freeze_deopt_exits(plans, component_plans, component_count, diag);
}

} // namespace zend::native::tpde::plan_detail
