// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/Common/zend_tpde_direct_user_call.hpp"

namespace zend::native::tpde {

IRValueRef ZendIRAdaptor::plan_source_operand_value_ref(
		const zend_tpde_plan *plan,
		const zend_mir_source_operand_ref &operand) {
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_id = zend_mir_value_from_synthetic(operand.index);
	} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.ssa_variable_id != ZEND_MIR_ID_INVALID) {
		value_id = zend_mir_value_from_original_ssa(
			operand.ssa_variable_id);
	}
	const int32_t index = zend_mir_id_is_valid(value_id)
		? zend_tpde_value_index(plan, value_id) : -1;
	return index < 0 ? INVALID_VALUE_REF
		: IRValueRef{MIR_VALUE_BASE + static_cast<uint32_t>(index)};
}

void ZendIRAdaptor::set_instruction_result(uint32_t index, IRValueRef value) {
	instruction_results_[index] = value;
	instruction_result_definitions_.add(index, value);
}

bool ZendIRAdaptor::frozen_effect_closed_inline(
		uint32_t instruction_index) const {
	return plan_ != nullptr
		&& plan_->effect_closed_inline_eligible != nullptr
		&& instruction_index < plan_->instruction_count
		&& plan_->effect_closed_inline_eligible[instruction_index] != 0;
}

void ZendIRAdaptor::override_ssa_value(uint32_t ssa_variable_id,
		IRValueRef value) {
	const int32_t index = zend_tpde_value_index(plan_,
		zend_mir_value_from_original_ssa(ssa_variable_id));
	if (index >= 0
			&& static_cast<uint32_t>(index)
				< active_value_overrides().size()) {
		active_value_overrides()[static_cast<uint32_t>(index)] = value;
	}
}

zend_mir_storage_id ZendIRAdaptor::source_operand_storage_id(
		const zend_mir_source_operand_ref &operand) const {
	if (operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return ZEND_MIR_ID_INVALID;
	}
	if (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
		return operand.index < plan_->source_frame_variable_count
			? operand.index : ZEND_MIR_ID_INVALID;
	}
	if ((operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& operand.index < plan_->source_temporary_count
			&& plan_->source_frame_variable_count
				<= ZEND_MIR_ID_MAX - operand.index) {
		return plan_->source_frame_variable_count + operand.index;
	}
	return ZEND_MIR_ID_INVALID;
}

bool ZendIRAdaptor::direct_internal_source_argument_stable(
		const zend_mir_call_argument_ref &argument,
		uint32_t call_source_position) const {
	const zend_mir_storage_id storage =
		source_operand_storage_id(argument.source_operand);
	if (!zend_mir_id_is_valid(storage)
			|| plan_->source_opcodes == nullptr
			|| argument.send_opline_index >= call_source_position
			|| call_source_position > plan_->source_opcode_count) {
		return false;
	}
	const uint32_t encoded_storage = EX_NUM_TO_VAR(storage);
	for (uint32_t source = argument.send_opline_index + 1;
			source < call_source_position; ++source) {
		const zend_tpde_source_opcode &opline =
			plan_->source_opcodes[source];
		const bool op1_slot = opline.op1_type == IS_CV
			|| opline.op1_type == IS_TMP_VAR
			|| opline.op1_type == IS_VAR;
		const bool op2_slot = opline.op2_type == IS_CV
			|| opline.op2_type == IS_TMP_VAR
			|| opline.op2_type == IS_VAR;
		const bool result_slot = opline.result_type == IS_CV
			|| opline.result_type == IS_TMP_VAR
			|| opline.result_type == IS_VAR;
		/*
		 * Every later mention counts, a read included: an earlier delayed
		 * TMP/VAR argument must not be consumed after a later SEND observed
		 * or reused the same slot.
		 */
		if ((op1_slot && opline.op1_var == encoded_storage)
				|| (op2_slot && opline.op2_var == encoded_storage)
				|| (result_slot
					&& opline.result_var == encoded_storage)) {
			return false;
		}
	}
	return true;
}

IRValueRef ZendIRAdaptor::mutation_value_ref(
		const zend_tpde_instruction &instruction) const {
	const uint32_t mutation_ssa =
		zend_tpde_register_mutation_ssa_plus_one(instruction, false);
	return mutation_ssa == 0
		? INVALID_VALUE_REF
		: value_ref(zend_mir_value_from_original_ssa(mutation_ssa - 1));
}

bool ZendIRAdaptor::storage_assigned_by_reference(
		zend_mir_storage_id storage_id) const {
	return zend_mir_id_is_valid(storage_id)
		&& std::ranges::binary_search(
			reference_assigned_storages_, storage_id);
}

/* The comparisons a long binary operation produces a boolean for. */
constexpr bool ZendIRAdaptor::long_comparison_source_opcode(uint32_t opcode) {
	return opcode == ZEND_IS_IDENTICAL
		|| opcode == ZEND_IS_NOT_IDENTICAL
		|| opcode == ZEND_IS_EQUAL
		|| opcode == ZEND_IS_NOT_EQUAL
		|| opcode == ZEND_IS_SMALLER
		|| opcode == ZEND_IS_SMALLER_OR_EQUAL;
}

/* The source opcodes of a binary operation on two longs. */
constexpr bool ZendIRAdaptor::long_binary_source_opcode(uint32_t opcode) {
	return opcode == ZEND_ADD
		|| opcode == ZEND_SUB
		|| opcode == ZEND_BW_OR
		|| opcode == ZEND_BW_AND
		|| opcode == ZEND_BW_XOR
		|| opcode == ZEND_SPACESHIP
		|| long_comparison_source_opcode(opcode);
}

bool ZendIRAdaptor::long_binary_machine_operands(
		const zend_tpde_instruction &instruction,
		IRValueRef &left, IRValueRef &right) const {
	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_BINARY_OP
			|| !long_binary_source_opcode(
				instruction.value_operation.source_opcode)) {
		return false;
	}
	/*
	 * Full DFA may coalesce a fetch result's source SSA identity back into
	 * its zval slot; the source binding still names the defining
	 * instruction and so its register boxed result. The source-operand
	 * lookup serves ordinary scalar plans.
	 */
	left = source_binding_value_ref(instruction.source_op1_binding);
	if (left == INVALID_VALUE_REF) {
		left = source_operand_value_ref(
			instruction.value_operation.op1);
	}
	right = source_binding_value_ref(instruction.source_op2_binding);
	if (right == INVALID_VALUE_REF) {
		right = source_operand_value_ref(
			instruction.value_operation.op2);
	}
	auto register_or_constant = [&](IRValueRef value) {
		if (machine_value_is_register_authoritative(value)) {
			/*
			 * A machine kind does not prove a TPDE definition: an
			 * unselected direct user call publishes its scalar result
			 * through the Zend slot although its plan value stays
			 * register-authoritative.
			 */
			return machine_value_has_register_definition(value);
		}
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->constant;
		}
		const uint32_t index = static_cast<uint32_t>(value);
		return is_mir_value(index)
			&& plan_->values[index - MIR_VALUE_BASE].constant;
	};
	auto long_register_value = [&](IRValueRef value) {
		return exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
			|| machine_kind(value)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	};
	const bool selected = left != INVALID_VALUE_REF
		&& right != INVALID_VALUE_REF
		&& long_register_value(left)
		&& long_register_value(right)
		&& register_or_constant(left)
		&& register_or_constant(right);
	return selected;
}

/* The address of a canonical frame slot, as a derived value, or
 * INVALID_VALUE_REF. */
IRValueRef ZendIRAdaptor::frame_slot_address(zend_mir_storage_id storage) {
	const uint32_t reference = zend_mir_id_is_valid(storage)
		? machine_reference_index(
			ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT, storage)
		: UINT32_MAX;
	return reference == UINT32_MAX ? INVALID_VALUE_REF
		: add_derived_value(ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE, storage, false, 0, UINT8_MAX,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED, ZEND_MIR_REFCOUNT_UNKNOWN,
			reference);
}

IRValueRef ZendIRAdaptor::add_derived_value(
		zend_mir_representation representation,
		zend_mir_scalar_type_mask exact_type,
		zend_mir_storage_id storage_id,
		bool constant, uint64_t constant_bits,
		uint8_t explicit_machine_kind,
		zend_mir_ownership_state ownership,
		zend_mir_refcount_state refcount_state,
		uint32_t machine_reference_index,
		bool known_string_literal,
		uint8_t known_string_first_byte,
		uint64_t known_string_length) {
	/* Derived values are bounded by the plan's operand and instruction
	 * counts, far below the 32-bit value-reference space. */
	ZEND_ASSERT(derived_values_.size()
		< UINT32_MAX - MIR_VALUE_BASE - plan_->value_count);
	zend_tpde_machine_value_kind kind = ZEND_TPDE_MACHINE_VALUE_I64;
	if (explicit_machine_kind != UINT8_MAX) {
		kind = static_cast<zend_tpde_machine_value_kind>(
			explicit_machine_kind);
	} else if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
		kind = ZEND_TPDE_MACHINE_VALUE_BOOL;
	} else if (exact_type == ZEND_MIR_SCALAR_TYPE_F64
			|| representation == ZEND_MIR_REPRESENTATION_DOUBLE) {
		kind = ZEND_TPDE_MACHINE_VALUE_F64;
	}
	const IRValueRef value{
		MIR_VALUE_BASE + plan_->value_count
			+ static_cast<uint32_t>(derived_values_.size())};
	derived_values_.push_back({
		representation, exact_type, storage_id, kind, ownership,
		refcount_state, machine_reference_index, constant, constant_bits,
		known_string_literal, known_string_first_byte,
		known_string_length});
	const uint32_t required_value_count =
		static_cast<uint32_t>(value) + 1;
	if (phi_input_slices_.size() < required_value_count) {
		phi_input_slices_.resize(required_value_count);
		phi_values_.resize(required_value_count);
	}
	if (machine_reference_index != UINT32_MAX) {
		machine_reference_values_.push_back(value);
	}
	return value;
}

/* An opcode of ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG. */
static constexpr bool executable_value_opcode(zend_mir_opcode opcode) {
	switch (opcode) {
#define EXECUTABLE_VALUE_CASE(symbol, label, value) \
		case ZEND_MIR_OPCODE_##symbol:
		ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG(EXECUTABLE_VALUE_CASE)
#undef EXECUTABLE_VALUE_CASE
			return true;
		default:
			return false;
	}
}

ZendIRAdaptor::InstKind ZendIRAdaptor::executable_kind(
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record) {
	if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
		return instruction.zval_store_lazy_scalar
			? InstKind::MIR : InstKind::ZvalStore;
	}
	if (!instruction.has_value_operation) {
		return InstKind::MIR;
	}
	switch (instruction.value_operation.opcode) {
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			if (instruction.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
				return InstKind::SlowPathCall;
			}
			return instruction.value_operation.op2.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
				? InstKind::ZvalMove : InstKind::ZvalCopy;
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			if (instruction.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_VAR) {
				return InstKind::SlowPathCall;
			}
			return instruction.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP
				? InstKind::ZvalMove : InstKind::ZvalCopy;
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
			return InstKind::ZvalCopy;
		case ZEND_MIR_OPCODE_VALUE_FREE:
			return InstKind::ZvalReleaseFast;
		/* The executable value operations lowered without their helper. */
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
			return InstKind::MIR;
		case ZEND_MIR_OPCODE_VALUE_ECHO:
		case ZEND_MIR_OPCODE_FUNC_GET_ARGS:
		case ZEND_MIR_OPCODE_VALUE_COUNT:
		case ZEND_MIR_OPCODE_VALUE_GET_TYPE:
		case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
		case ZEND_MIR_OPCODE_VALUE_IN_ARRAY:
		case ZEND_MIR_OPCODE_VALUE_ISSET_THIS:
		case ZEND_MIR_OPCODE_VALUE_GET_CALLED_CLASS:
		case ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE:
		case ZEND_MIR_OPCODE_VALUE_END_SILENCE:
		case ZEND_MIR_OPCODE_VALUE_MATCH_ERROR:
		case ZEND_MIR_OPCODE_VALUE_VERIFY_NEVER_TYPE:
		case ZEND_MIR_OPCODE_VALUE_DEFINED:
		case ZEND_MIR_OPCODE_VALUE_TICKS:
		case ZEND_MIR_OPCODE_VALUE_TYPE_ASSERT:
		case ZEND_MIR_OPCODE_VALUE_EXT_STMT:
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_BEGIN:
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_END:
		case ZEND_MIR_OPCODE_VALUE_EXT_NOP:
		case ZEND_MIR_OPCODE_VALUE_DISCARD_EXCEPTION:
		case ZEND_MIR_OPCODE_VALUE_CHECK_FUNC_ARG:
		case ZEND_MIR_OPCODE_VALUE_CHECK_UNDEF_ARGS:
			return InstKind::SlowPathCall;
		default:
			break;
	}
	/* Every other executable value operation calls its helper. */
	return executable_value_opcode(instruction.value_operation.opcode)
		? InstKind::SlowPathCall : InstKind::MIR;
}

ZendIRAdaptor::ScalarTypeCheckSelection
ZendIRAdaptor::scalar_type_check_selection(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction,
		IRValueRef *input_out,
		IRValueRef *result_out) {
	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| instruction.value_operation.source_opcode
				!= ZEND_TYPE_CHECK) {
		return ScalarTypeCheckSelection::Invalid;
	}
	const IRValueRef input = plan_source_operand_value_ref(
		plan, instruction.value_operation.op1);
	const IRValueRef result = plan_source_operand_value_ref(
		plan, instruction.value_operation.result);
	const uint32_t input_index = static_cast<uint32_t>(input);
	const uint32_t result_index = static_cast<uint32_t>(result);
	if (input == INVALID_VALUE_REF
			|| input_index < MIR_VALUE_BASE
			|| input_index - MIR_VALUE_BASE >= plan->value_count
			|| instruction.value_operation.result.ssa_variable_id
				== ZEND_MIR_ID_INVALID
			|| (result != INVALID_VALUE_REF
				&& (result_index < MIR_VALUE_BASE
					|| result_index - MIR_VALUE_BASE
						>= plan->value_count))) {
		return ScalarTypeCheckSelection::Invalid;
	}
	if (input_out != nullptr) {
		*input_out = input;
	}
	if (result_out != nullptr) {
		*result_out = result;
	}
	const zend_mir_scalar_type_mask input_type =
		plan->values[input_index - MIR_VALUE_BASE].exact_type;
	const uint32_t expected = instruction.value_operation.extended_value;
	if (input_type == ZEND_MIR_SCALAR_TYPE_I1) {
		const uint32_t bool_mask = expected & MAY_BE_BOOL;
		if (bool_mask == MAY_BE_TRUE) {
			return ScalarTypeCheckSelection::CopyInput;
		}
		if (bool_mask == MAY_BE_FALSE) {
			return ScalarTypeCheckSelection::NotInput;
		}
		return bool_mask == MAY_BE_BOOL
			? ScalarTypeCheckSelection::ConstantTrue
			: ScalarTypeCheckSelection::ConstantFalse;
	}
	uint32_t input_mask;
	switch (input_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			input_mask = MAY_BE_NULL;
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			input_mask = MAY_BE_LONG;
			break;
		case ZEND_MIR_SCALAR_TYPE_F64:
			input_mask = MAY_BE_DOUBLE;
			break;
		default:
			return ScalarTypeCheckSelection::Invalid;
	}
	return (expected & input_mask) != 0
		? ScalarTypeCheckSelection::ConstantTrue
		: ScalarTypeCheckSelection::ConstantFalse;
}

bool ZendIRAdaptor::machine_value_has_frozen_use(uint32_t value_index) const {
	if (value_index >= plan_->value_count) {
		return false;
	}
	const uint8_t *required =
		function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_value_required
			: plan_->entry_value_required;
	return required != nullptr && required[value_index] != 0;
}

/*
 * The frame-state map of each deoptimization landing (ADR 0025 section
 * 1): every machine value live into the landing, which its stub loads
 * from the value's canonical frame slot into the place TPDE keeps it at
 * the block start. Machine liveness, not MIR liveness: values the MIR
 * does not name (unboxed arguments, loaded payloads) belong to the map
 * too. A landing with a live value that its slot cannot reproduce is
 * dropped: no transfer resumes there.
 */
void ZendIRAdaptor::freeze_deopt_landing_values(
		const std::vector<BlockItem<IRInstRef>> &block_instructions,
		const std::vector<BlockItem<IRValueRef>> &block_phis,
		uint32_t block_count) {
	const bool exits = std::ranges::any_of(nodes_,
		[](const InstNode &current) { return current.deopt_exit; });
	if ((plan_->deopt_resume_count == 0 && !exits) || !valid_
			|| function_mode_ != FunctionMode::ZendEntry) {
		return;
	}
	const uint32_t value_count = MIR_VALUE_BASE + plan_->value_count
		+ static_cast<uint32_t>(derived_values_.size());
	const uint32_t words = (value_count + 63) / 64;
	if (uint64_t{words} * block_count > (uint64_t{1} << 22)) {
		return;
	}
	auto tracked = [&](IRValueRef value) {
		const uint32_t index = static_cast<uint32_t>(value);
		uint64_t bits;
		return index >= MIR_VALUE_BASE && index < value_count
			&& !constant(value, &bits)
			&& !machine_reference(value, nullptr);
	};
	std::vector<std::vector<uint32_t>> block_nodes(block_count);
	for (const BlockItem<IRInstRef> &item : block_instructions) {
		if (item.block < block_count) {
			block_nodes[item.block].push_back(
				static_cast<uint32_t>(item.value));
		}
	}
	std::vector<uint8_t> definition_kind(value_count, UINT8_MAX);
	std::vector<uint32_t> definition_node(value_count, UINT32_MAX);
	auto node_operands = [&](const InstNode &current) {
		return std::span<const IRValueRef>{operands_}.subspan(
			current.operand_offset, current.operand_count);
	};
	std::vector<uint64_t> use(uint64_t{words} * block_count, 0);
	std::vector<uint64_t> def(uint64_t{words} * block_count, 0);
	std::vector<uint64_t> phi_def(uint64_t{words} * block_count, 0);
	std::vector<uint64_t> phi_use(uint64_t{words} * block_count, 0);
	auto set = [&](std::vector<uint64_t> &bits, uint32_t block,
			IRValueRef value) {
		const uint32_t index = static_cast<uint32_t>(value);
		bits[uint64_t{block} * words + index / 64] |=
			uint64_t{1} << (index % 64);
	};
	auto test = [&](const std::vector<uint64_t> &bits, uint32_t block,
			uint32_t index) {
		return (bits[uint64_t{block} * words + index / 64]
			>> (index % 64)) & 1;
	};
	for (uint32_t block = 0; block < block_count; ++block) {
		for (const uint32_t node_index : block_nodes[block]) {
			const InstNode &current = nodes_[node_index];
			for (const IRValueRef operand : node_operands(current)) {
				if (tracked(operand)
						&& !test(def, block,
							static_cast<uint32_t>(operand))) {
					set(use, block, operand);
				}
			}
			if (current.has_result && tracked(current.result)) {
				const uint32_t result =
					static_cast<uint32_t>(current.result);
				set(def, block, current.result);
				definition_kind[result] =
					static_cast<uint8_t>(current.kind);
				definition_node[result] = node_index;
			}
		}
	}
	for (const BlockItem<IRValueRef> &phi : block_phis) {
		if (phi.block >= block_count || !tracked(phi.value)) {
			continue;
		}
		set(phi_def, phi.block, phi.value);
		const Slice &slice =
			phi_input_slices_[static_cast<uint32_t>(phi.value)];
		for (uint32_t n = 0; n < slice.count; ++n) {
			const PhiInput &input = phi_inputs_[slice.offset + n];
			const uint32_t from = static_cast<uint32_t>(input.block);
			if (from < block_count && tracked(input.value)) {
				set(phi_use, from, input.value);
			}
		}
	}
	std::vector<uint64_t> live_in(uint64_t{words} * block_count, 0);
	std::vector<uint64_t> live_out(uint64_t{words} * block_count, 0);
	for (bool changed = true; changed;) {
		changed = false;
		for (uint32_t block = block_count; block-- > 0;) {
			const Slice &slice = successor_slices_[block];
			for (uint32_t word = 0; word < words; ++word) {
				uint64_t out = phi_use[uint64_t{block} * words + word];
				for (uint32_t n = 0; n < slice.count; ++n) {
					const uint32_t successor = static_cast<uint32_t>(
						successors_[slice.offset + n]);
					out |= live_in[uint64_t{successor} * words + word]
						& ~phi_def[uint64_t{successor} * words + word];
				}
				const uint64_t in =
					use[uint64_t{block} * words + word]
					| (out & ~def[uint64_t{block} * words + word]);
				if (out != live_out[uint64_t{block} * words + word]
						|| in != live_in[uint64_t{block} * words + word]) {
					live_out[uint64_t{block} * words + word] = out;
					live_in[uint64_t{block} * words + word] = in;
					changed = true;
				}
			}
		}
	}
	/* The values defined on every path into a block (must-available).
	 * A value live into a block but not available there is used only
	 * on a path that defines it first, such as an exception edge into
	 * a finally block; no landing reproduces it. */
	std::vector<uint64_t> available(uint64_t{words} * block_count,
		~uint64_t{0});
	std::vector<std::vector<uint32_t>> predecessors(block_count);
	for (uint32_t block = 0; block < block_count; ++block) {
		const Slice &slice = successor_slices_[block];
		for (uint32_t n = 0; n < slice.count; ++n) {
			const uint32_t successor = static_cast<uint32_t>(
				successors_[slice.offset + n]);
			if (successor < block_count) {
				predecessors[successor].push_back(block);
			}
		}
	}
	const uint32_t entry_block = static_cast<uint32_t>(
		block_index(plan_->function.entry_block_id));
	for (bool changed = true; changed;) {
		changed = false;
		for (uint32_t block = 0; block < block_count; ++block) {
			for (uint32_t word = 0; word < words; ++word) {
				uint64_t in = block == entry_block
						|| predecessors[block].empty()
					? 0 : ~uint64_t{0};
				for (const uint32_t predecessor : predecessors[block]) {
					in &= available[uint64_t{predecessor} * words + word];
				}
				const uint64_t out = in
					| def[uint64_t{block} * words + word]
					| phi_def[uint64_t{block} * words + word];
				if (out != available[uint64_t{block} * words + word]) {
					available[uint64_t{block} * words + word] = out;
					changed = true;
				}
			}
		}
	}
	auto available_into = [&](uint32_t block, uint32_t index) {
		if (block == entry_block || predecessors[block].empty()) {
			return false;
		}
		for (const uint32_t predecessor : predecessors[block]) {
			if (!test(available, predecessor, index)) {
				return false;
			}
		}
		return true;
	};
	/* A value the landing can define again from its frame slot: a MIR
	 * value whose slot holds it (not through a reference), a payload
	 * loaded directly from its slot, or a derived value of a CV, whose
	 * slot assignments write (a scalar's the transfer stores). A derived
	 * temporary is not: its slot may already be consumed, as by an
	 * assignment that moved it into a CV the value now stands for. */
	auto reloadable = [&](uint32_t index) {
		const IRValueRef value{index};
		const zend_mir_storage_id storage = canonical_storage(value);
		if (!zend_mir_id_is_valid(storage)) {
			return false;
		}
		const zend_tpde_machine_value_kind kind = machine_kind(value);
		if (index < MIR_VALUE_BASE + plan_->value_count) {
			const zend_tpde_value &plan_value =
				plan_->values[index - MIR_VALUE_BASE];
			return !plan_value.canonical_alias_observable
				|| kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		}
		if (storage < plan_->source_frame_variable_count) {
			return kind != ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		}
		if (definition_kind[index]
				!= static_cast<uint8_t>(InstKind::ZvalPayloadLoad)
				|| definition_node[index] == UINT32_MAX) {
			return false;
		}
		const InstNode &load = nodes_[definition_node[index]];
		zend_mir_storage_id loaded = ZEND_MIR_ID_INVALID;
		return load.operand_count == 1
			&& frame_slot_reference(
				operands_[load.operand_offset], &loaded)
			&& loaded == storage;
	};
	/* The landing of a resume ID is the start of its operation's cold
	 * block, where the generic operation begins; its map is the block's
	 * live-in set. */
	deopt_resume_landed_.assign(plan_->deopt_resume_count, 0);
	deopt_landing_slices_.assign(plan_->deopt_resume_count, {});
	/* Per resumed instruction: the first block holding its operation's
	 * cold node, and all of its cold nodes in node order. */
	std::unordered_map<uint32_t, uint32_t> cold_blocks;
	std::unordered_map<uint32_t, std::vector<uint32_t>> cold_nodes;
	for (uint32_t resume = 0; resume < plan_->deopt_resume_count;
			++resume) {
		cold_blocks.try_emplace(
			plan_->deopt_resume_instructions[resume], UINT32_MAX);
		cold_nodes.try_emplace(plan_->deopt_resume_instructions[resume]);
	}
	for (uint32_t block = 0; block < block_count; ++block) {
		for (const uint32_t node_index : block_nodes[block]) {
			const InstNode &current = nodes_[node_index];
			if (current.kind != InstKind::GuardedCold
					|| current.synthetic) {
				continue;
			}
			const auto found =
				cold_blocks.find(current.mir_instruction_index);
			if (found != cold_blocks.end()
					&& found->second == UINT32_MAX) {
				found->second = block;
			}
		}
	}
	for (uint32_t node_index = 0; node_index < nodes_.size();
			++node_index) {
		const InstNode &current = nodes_[node_index];
		if (current.kind != InstKind::GuardedCold || current.synthetic) {
			continue;
		}
		const auto found = cold_nodes.find(current.mir_instruction_index);
		if (found != cold_nodes.end()) {
			found->second.push_back(node_index);
		}
	}
	for (uint32_t resume = 0; resume < plan_->deopt_resume_count;
			++resume) {
		const uint32_t instruction = plan_->deopt_resume_instructions[resume];
		const uint32_t cold_block = cold_blocks[instruction];
		if (cold_block == UINT32_MAX || block_nodes[cold_block].empty()) {
			continue;
		}
		const uint32_t offset =
			static_cast<uint32_t>(deopt_landing_values_.size());
		bool complete = true;
		/* The live-in values in ascending order. */
		for (uint32_t word = 0; word < words && complete; ++word) {
			for (uint64_t bits =
						live_in[uint64_t{cold_block} * words + word];
					bits != 0; bits &= bits - 1) {
				const uint32_t index = word * 64
					+ static_cast<uint32_t>(std::countr_zero(bits));
				if (index >= value_count) {
					break;
				}
				if (!available_into(cold_block, index)) {
					continue;
				}
				if (!reloadable(index)) {
					complete = false;
					break;
				}
				deopt_landing_values_.push_back(IRValueRef{index});
			}
		}
		if (!complete) {
			deopt_landing_values_.erase(
				deopt_landing_values_.begin() + offset,
				deopt_landing_values_.end());
			continue;
		}
		deopt_landing_slices_[resume] = {offset,
			static_cast<uint32_t>(deopt_landing_values_.size()) - offset};
		deopt_resume_landed_[resume] = 1;
		nodes_[block_nodes[cold_block].front()].deopt_landing = resume;
		add_deopt_stores(cold_nodes[instruction],
			std::span<const IRValueRef>{deopt_landing_values_}.subspan(
				offset, deopt_landing_slices_[resume].count));
	}
	/* A specialized member's exit stores the register scalars its frame
	 * would carry into the operation's continuation: the generic copy's
	 * landing reloads them from their slots. */
	for (uint32_t block = 0; block < block_count; ++block) {
		for (const uint32_t node_index : block_nodes[block]) {
			InstNode &exit = nodes_[node_index];
			if (!exit.deopt_exit
					|| exit.continuation_block >= block_count) {
				continue;
			}
			std::vector<IRValueRef> stores;
			for (uint32_t index = 0; index < value_count; ++index) {
				if ((!test(live_in, exit.continuation_block, index)
							&& !test(live_in, block, index))
						|| !available_into(block, index)
						|| !zend_mir_id_is_valid(
							canonical_storage(IRValueRef{index}))) {
					continue;
				}
				if (zend_tpde_machine_kind_is_scalar(
						machine_kind(IRValueRef{index}))) {
					stores.push_back(IRValueRef{index});
				}
			}
			attach_deopt_stores(exit, stores);
		}
	}
}

/*
 * A register scalar's slot may be stale where an operation deoptimizes
 * (a lazily written CV, a typed-tier value). The operation's slow node,
 * which runs the transfer, first stores every scalar of the landing's
 * map: a slot whose value is a scalar holds no counted value, so the
 * store is exact. Pointer values stay with their slots, which a
 * separating helper may have replaced. cold_nodes are the operation's
 * cold nodes in node order.
 */
void ZendIRAdaptor::add_deopt_stores(const std::vector<uint32_t> &cold_nodes,
		std::span<const IRValueRef> landing_values) {
	std::vector<IRValueRef> stores;
	for (const IRValueRef value : landing_values) {
		if (zend_tpde_machine_kind_is_scalar(machine_kind(value))) {
			stores.push_back(value);
		}
	}
	for (const uint32_t node_index : cold_nodes) {
		attach_deopt_stores(nodes_[node_index], stores);
	}
}

/* Appends stores as liveness operands a node stores first
 * (deopt_store_operand_index). */
void ZendIRAdaptor::attach_deopt_stores(InstNode &current,
		const std::vector<IRValueRef> &stores) {
	if (stores.empty()) {
		return;
	}
	const uint32_t old_offset = current.operand_offset;
	const uint32_t old_count = current.operand_count;
	if (current.semantic_operand_count == UINT32_MAX) {
		current.semantic_operand_count =
			current.materialization_operand_index == UINT32_MAX
				? old_count : current.materialization_operand_index;
	}
	const uint32_t new_offset =
		static_cast<uint32_t>(operands_.size());
	for (uint32_t operand = 0; operand < old_count; ++operand) {
		operands_.push_back(operands_[old_offset + operand]);
	}
	operands_.insert(operands_.end(), stores.begin(), stores.end());
	current.operand_offset = new_offset;
	current.operand_count = old_count
		+ static_cast<uint32_t>(stores.size());
	current.deopt_store_operand_index = old_count;
	current.deopt_store_count = static_cast<uint32_t>(stores.size());
}

/*
 * Moves the operands appended since operand_offset behind a new
 * operand range: their old entries stay unused.
 */
void ZendIRAdaptor::move_operand_prefix(uint32_t &operand_offset,
		uint32_t end) {
	const uint32_t prefix_offset = operand_offset;
	operand_offset = static_cast<uint32_t>(operands_.size());
	for (uint32_t operand = prefix_offset; operand < end; ++operand) {
		const IRValueRef value = operands_[operand];
		operands_.push_back(value);
	}
}

/*
 * Adds a load from the frame slot at address before the operation whose
 * operands start at operand_offset: the load takes the next operand, and
 * the operation's operands so far move behind it.
 */
void ZendIRAdaptor::add_payload_load_before(
		std::vector<BlockItem<IRInstRef>> &block_instructions,
		uint32_t block, uint32_t instruction, IRValueRef address,
		IRValueRef loaded, zend_mir_storage_id storage_id,
		zend_mir_scalar_type_mask type, uint32_t &operand_offset) {
	const uint32_t prefix_end = static_cast<uint32_t>(operands_.size());
	const uint32_t load_operand_offset = prefix_end;
	operands_.push_back(address);
	add_node(block_instructions, block, InstNode{
		.kind = InstKind::ZvalPayloadLoad,
		.mir_instruction_index = instruction,
		.argument_index = UINT32_MAX,
		.result = loaded,
		.operand_offset = load_operand_offset,
		.operand_count = 1,
		.has_result = true,
		.storage_id = storage_id,
		.exact_type = type});
	move_operand_prefix(operand_offset, prefix_end);
}

void ZendIRAdaptor::add_node(
		std::vector<BlockItem<IRInstRef>> &block_instructions,
		uint32_t block, InstNode node) {
	if (node.control_block == UINT32_MAX) {
		node.control_block = block;
	}
	uint32_t index = static_cast<uint32_t>(nodes_.size());
	nodes_.push_back(std::move(node));
	fused_instructions_.push_back(0);
	block_instructions.push_back({block, IRInstRef{index}});
}

template <typename T>
void ZendIRAdaptor::flatten_block_items(
		uint32_t block_count,
		const std::vector<BlockItem<T>> &items,
		std::vector<Slice> &slices,
		std::vector<T> &values) {
	slices.assign(block_count, {});
	for (const BlockItem<T> &item : items) {
		++slices[item.block].count;
	}
	uint32_t offset = 0;
	for (Slice &slice : slices) {
		slice.offset = offset;
		offset += slice.count;
		slice.count = 0;
	}
	values.clear();
	if (offset != 0) {
		values.assign(offset, items.front().value);
	}
	for (const BlockItem<T> &item : items) {
		Slice &slice = slices[item.block];
		values[slice.offset + slice.count++] = item.value;
	}
}

/*
 * The clone inline_component_scalar_body() builds, which its steps share.
 */
struct ZendIRAdaptor::InlineClone {
	const zend_tpde_instruction &call;
	const uint32_t caller_instruction_index;
	const IRValueRef caller_result;
	const uint32_t caller_block;
	std::vector<BlockItem<IRInstRef>> &block_instructions;
	std::vector<IRValueRef> &values;
	std::vector<int32_t> &checked_value_steps;
	const zend_tpde_plan *callee = nullptr;
	zend_tpde_inline_body_shape shape{};
	bool scalar_diamond = false;
	const zend_tpde_scalar_diamond &diamond = shape.diamond;
	size_t derived_value_checkpoint = 0;
	size_t machine_reference_checkpoint = 0;
	size_t operand_checkpoint = 0;
	size_t node_checkpoint = 0;
	size_t fused_checkpoint = 0;
	size_t block_instruction_checkpoint = 0;
	IRValueRef returned = INVALID_VALUE_REF;
	int32_t returned_checked_step = -1;
	bool &saw_return = shape.saw_return;
	zend_mir_value_id deferred_return_id = ZEND_MIR_ID_INVALID;
	zend_tpde_source_value_binding deferred_return_binding{};
	bool deferred_return_has_binding = false;
	IRValueRef diamond_condition = INVALID_VALUE_REF;
	std::vector<uint32_t> pending_phis;
	std::vector<IRValueRef> checked_operands;
	std::vector<InlinedCheckedStep> checked_steps;
	bool cloned_string_length = false;
};

/*
 * Clones a proof-closed scalar callee body into the caller graph after
 * every component member was lowered: source opcodes are not consulted, and
 * the target backends see ordinary TPDE instructions and values. Only a
 * call the plan froze as an effect-closed clone gets here, so the plan's
 * facts for it hold: its guarded cold block, its argument bindings and the
 * arguments the body reads. The observer-enabled path still runs the complete Zend-frame call, so pure
 * scalar instructions may run speculatively before the observer guard; no
 * effectful, trapping, branching or frame-observing instruction is
 * admitted.
 */
ZendIRAdaptor::InlinedBody ZendIRAdaptor::inline_component_scalar_body(
		const zend_tpde_instruction &call,
		uint32_t caller_instruction_index,
		IRValueRef caller_result,
		uint32_t caller_block,
		std::vector<BlockItem<IRInstRef>> &block_instructions) {
	if (call.component_target_index == UINT32_MAX
			|| call.component_target_index >= component_plans_.size()) {
		return {};
	}
	InlineClone clone{
		.call = call,
		.caller_instruction_index = caller_instruction_index,
		.caller_result = caller_result,
		.caller_block = caller_block,
		.block_instructions = block_instructions,
		.values = inline_body_values_,
		.checked_value_steps = inline_body_checked_value_steps_};
	clone.callee =
		component_plans_[clone.call.component_target_index];
	/* The cloner applies no instruction limit of its own. */
	clone.shape.semantic_instruction_limit = UINT32_MAX;
	if (clone.callee == plan_
			|| (clone.callee != nullptr
				&& clone.callee->argument_count
					!= clone.call.call_argument_count)
			|| !zend_tpde_inline_body_begin(
				clone.call, clone.callee, &clone.shape)) {
		return {};
	}
	clone.scalar_diamond = clone.shape.scalar_diamond;

	clone.values.assign(clone.callee->value_count, INVALID_VALUE_REF);
	clone.checked_value_steps.assign(clone.callee->value_count, -1);
	clone.derived_value_checkpoint = derived_values_.size();
	clone.machine_reference_checkpoint =
		machine_reference_values_.size();
	clone.operand_checkpoint = operands_.size();
	clone.node_checkpoint = nodes_.size();
	clone.fused_checkpoint = fused_instructions_.size();
	clone.block_instruction_checkpoint =
		clone.block_instructions.size();
	for (uint32_t argument_index = 0;
			argument_index < clone.callee->argument_count; ++argument_index) {
		if (!map_inline_argument(clone, argument_index)) {
			return fail_inline(clone);
		}
	}

	for (uint32_t index = 0; index < clone.callee->instruction_count; ++index) {
		if (!clone_inline_instruction(clone, index)) {
			return fail_inline(clone);
		}
	}
	if (clone.scalar_diamond && !select_inline_diamond(clone)) {
		return fail_inline(clone);
	}
	if (!clone.saw_return
			|| (clone.returned == INVALID_VALUE_REF
				&& clone.returned_checked_step < 0)
			|| (clone.returned != INVALID_VALUE_REF
				&& exact_type(clone.returned)
					!= clone.call.direct_call->result_type)) {
		return fail_inline(clone);
	}
	if (!clone.checked_steps.empty()) {
		if (clone.returned != INVALID_VALUE_REF
				|| clone.returned_checked_step
					!= static_cast<int32_t>(clone.checked_steps.size() - 1)) {
			return fail_inline(clone);
		}
		InlinedBody body;
		body.valid = true;
		body.value = clone.caller_result;
		body.checked_operands = std::move(clone.checked_operands);
		body.checked_steps = std::move(clone.checked_steps);
		return body;
	}
	if (clone.cloned_string_length) {
		/*
		 * The cloned unary operation owns a fresh SSA result, which
		 * CALL_DIRECT_USER consumes once. The path-separating copy below is
		 * needed only when the fast result aliases a caller value the
		 * materialized call path also observes.
		 */
		return {true, clone.returned};
	}
	/*
	 * The inlined fast result stays distinct from the caller value the
	 * materialized observer path consumes: TPDE sees one SSA use on each
	 * side instead of two path-sensitive uses of one ValueAssignment inside
	 * CALL_DIRECT_USER.
	 */
	const uint32_t result_operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.push_back(clone.returned);
	const IRValueRef inline_result = add_derived_value(
		representation(clone.returned), exact_type(clone.returned),
		ZEND_MIR_ID_INVALID);
	zend_mir_instruction_record copy_record{};
	copy_record.opcode = ZEND_MIR_OPCODE_COPY;
	copy_record.block_id = clone.callee->block_ids[0];
	add_node(clone.block_instructions, clone.caller_block, InstNode{
		.kind = InstKind::MIR,
		.mir_instruction_index = UINT32_MAX,
		.argument_index = UINT32_MAX,
		.result = inline_result,
		.operand_offset = result_operand_offset,
		.operand_count = 1,
		.has_result = true,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.synthetic = true,
		.synthetic_record = copy_record});
	return {true, inline_result};
}

ZendIRAdaptor::InlinedBody ZendIRAdaptor::fail_inline(InlineClone &clone) {
	derived_values_.erase(
		derived_values_.begin() + clone.derived_value_checkpoint,
		derived_values_.end());
	machine_reference_values_.erase(
		machine_reference_values_.begin()
			+ clone.machine_reference_checkpoint,
		machine_reference_values_.end());
	operands_.erase(operands_.begin() + clone.operand_checkpoint,
		operands_.end());
	nodes_.erase(nodes_.begin() + clone.node_checkpoint, nodes_.end());
	fused_instructions_.erase(
		fused_instructions_.begin() + clone.fused_checkpoint,
		fused_instructions_.end());
	clone.block_instructions.erase(
		clone.block_instructions.begin() + clone.block_instruction_checkpoint,
		clone.block_instructions.end());
	return {};
}

bool ZendIRAdaptor::map_inline_argument(
		InlineClone &clone, uint32_t argument_index) {
	const int32_t callee_value_index =
		clone.callee->argument_value_indices == nullptr
		? -1 : clone.callee->argument_value_indices[argument_index];
	zend_mir_call_argument_ref argument;
	if (callee_value_index < 0
			|| static_cast<uint32_t>(callee_value_index)
				>= clone.callee->value_count
			|| !zend_tpde_call_argument_at(plan_,
				clone.call.call_argument_offset + argument_index,
				&argument)) {
		return false;
	}
	/* Neither transport nor unbox an argument an effect-closed body
	 * never reads; the planner left it unchecked too. */
	if (clone.callee->effect_closed_argument_used != nullptr
			&& clone.callee->effect_closed_argument_used[argument_index] == 0) {
		return true;
	}
	const zend_tpde_value &callee_value =
		clone.callee->values[static_cast<uint32_t>(callee_value_index)];
	const TypedBodyAbiType callee_argument_abi =
		typed_body_value_abi(clone.callee,
			static_cast<uint32_t>(callee_value_index));
	const zend_mir_representation expected_representation =
		callee_argument_abi.valid
			? callee_argument_abi.representation
			: callee_value.representation;
	const zend_mir_scalar_type_mask expected_exact_type =
		callee_argument_abi.valid
			? callee_argument_abi.exact_type
			: callee_value.exact_type;
	const zend_tpde_machine_value_kind expected_machine_kind =
		callee_argument_abi.valid
			? callee_argument_abi.machine_kind
			: callee_value.machine_kind;
	IRValueRef caller_value = INVALID_VALUE_REF;
	if (plan_->call_argument_bindings != nullptr) {
		caller_value = source_binding_value_ref(
			plan_->call_argument_bindings[
				clone.call.call_argument_offset + argument_index]);
	}
	if (caller_value == INVALID_VALUE_REF) {
		caller_value =
			source_operand_value_ref(argument.source_operand);
	}
	if (zend_mir_id_is_valid(argument.value_id)
			&& caller_value == INVALID_VALUE_REF) {
		caller_value = value_ref(argument.value_id);
	} else if (caller_value == INVALID_VALUE_REF) {
		const zend_native_direct_call_argument &descriptor_argument =
			clone.call.direct_call->arguments[argument_index];
		if (!zend_mir_scalar_type_is_exact(
				descriptor_argument.exact_type)
				|| descriptor_argument.exact_type
					== ZEND_MIR_SCALAR_TYPE_NULL
				|| descriptor_argument.exact_type
					== ZEND_MIR_SCALAR_TYPE_F64
				|| descriptor_argument.exact_type
					!= expected_exact_type) {
			return true;
		}
		caller_value = add_derived_value(
			expected_representation,
			descriptor_argument.exact_type,
			ZEND_MIR_ID_INVALID, true,
			descriptor_argument.scalar_bits);
	}
	if (caller_value != INVALID_VALUE_REF
			&& machine_kind(caller_value)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& zend_tpde_machine_kind_is_long_or_bool(
				expected_machine_kind)
			&& (exact_type(caller_value) == expected_exact_type
				|| exact_type(caller_value)
					== ZEND_MIR_SCALAR_TYPE_NONE)
			&& machine_value_is_register_authoritative(
				caller_value)) {
		const IRValueRef transported = add_derived_value(
			expected_representation, expected_exact_type,
			canonical_storage(caller_value), false, 0,
			expected_machine_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN);
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(caller_value);
		add_node(clone.block_instructions, clone.caller_block, InstNode{
			.kind = InstKind::UnboxScalar,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = transported,
			.operand_offset = operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = canonical_storage(caller_value),
			.exact_type = expected_exact_type});
		caller_value = transported;
	}
	if (caller_value != INVALID_VALUE_REF
			&& machine_kind(caller_value)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& zend_tpde_machine_kind_is_long_or_bool(
				expected_machine_kind)
			&& (exact_type(caller_value) == expected_exact_type
				|| exact_type(caller_value)
					== ZEND_MIR_SCALAR_TYPE_NONE)
			&& !machine_value_is_register_authoritative(
				caller_value)) {
		const zend_mir_storage_id storage_id =
			canonical_storage(caller_value);
		const IRValueRef address = frame_slot_address(storage_id);
		const IRValueRef transported = address != INVALID_VALUE_REF
			? add_derived_value(
				expected_representation, expected_exact_type,
				storage_id, false, 0, expected_machine_kind,
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_UNKNOWN)
			: INVALID_VALUE_REF;
		if (transported == INVALID_VALUE_REF) {
			return false;
		}
		const uint32_t load_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(address);
		add_node(clone.block_instructions, clone.caller_block, InstNode{
			.kind = InstKind::ZvalPayloadLoad,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = transported,
			.operand_offset = load_operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = storage_id,
			.exact_type = expected_exact_type});
		caller_value = transported;
	}
	if (caller_value != INVALID_VALUE_REF
			&& machine_kind(caller_value)
				== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
			&& expected_machine_kind
				== ZEND_TPDE_MACHINE_VALUE_I64
			&& clone.call.direct_call->arguments[argument_index].exact_type
				== expected_exact_type
			&& machine_value_is_register_authoritative(caller_value)) {
		const IRValueRef transported = add_derived_value(
			expected_representation, expected_exact_type,
			canonical_storage(caller_value), false, 0,
			expected_machine_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN);
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(caller_value);
		add_node(clone.block_instructions, clone.caller_block, InstNode{
			.kind = InstKind::UnboxReferenceScalar,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = transported,
			.operand_offset = operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = canonical_storage(caller_value),
			.exact_type = expected_exact_type});
		caller_value = transported;
	}
	if (caller_value != INVALID_VALUE_REF
			&& machine_kind(caller_value)
				== expected_machine_kind
			&& zend_tpde_machine_kind_is_pointer(expected_machine_kind)
			&& !machine_value_is_register_authoritative(
				caller_value)) {
		const zend_mir_storage_id storage_id =
			canonical_storage(caller_value);
		const IRValueRef address =
			frame_slot_address(storage_id);
		const IRValueRef transported =
			address != INVALID_VALUE_REF
				? add_derived_value(
					expected_representation,
					expected_exact_type,
					storage_id, false, 0,
					expected_machine_kind,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					callee_value.refcount_state)
				: INVALID_VALUE_REF;
		if (transported == INVALID_VALUE_REF) {
			return false;
		}
		const uint32_t load_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(address);
		add_node(clone.block_instructions, clone.caller_block, InstNode{
			.kind = InstKind::ZvalPayloadLoad,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = transported,
			.operand_offset = load_operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = storage_id,
			.exact_type = callee_value.exact_type});
		caller_value = transported;
	}
	if (caller_value == INVALID_VALUE_REF
			|| exact_type(caller_value)
				!= expected_exact_type
			|| machine_kind(caller_value)
				!= expected_machine_kind
			|| representation(caller_value)
				!= expected_representation
			|| exact_type(caller_value)
				== ZEND_MIR_SCALAR_TYPE_F64) {
		/*
		 * An effect-closed body does not transport arguments it never
		 * consumes: an ABI-incompatible unused parameter stays
		 * unmapped, and a real use below still fails the clone through
		 * mapped_value().
		 */
		return true;
	}
	clone.values[static_cast<uint32_t>(callee_value_index)] =
		caller_value;
	return true;
}

IRValueRef ZendIRAdaptor::mapped_value(
		InlineClone &clone, zend_mir_value_id value_id) {
	int32_t value_index =
		zend_tpde_value_index(clone.callee, value_id);
	if (value_index < 0) {
		return INVALID_VALUE_REF;
	}
	for (uint32_t depth = 0;
			depth < clone.callee->value_count; ++depth) {
		IRValueRef &mapped =
			clone.values[static_cast<uint32_t>(value_index)];
		if (mapped != INVALID_VALUE_REF) {
			return mapped;
		}
		const int32_t alias =
			clone.callee->values[static_cast<uint32_t>(value_index)]
				.register_alias_value_index;
		if (alias < 0 || alias == value_index) {
			break;
		}
		if (static_cast<uint32_t>(alias)
				>= clone.callee->value_count) {
			return INVALID_VALUE_REF;
		}
		value_index = alias;
	}
	IRValueRef &mapped = clone.values[static_cast<uint32_t>(value_index)];
	if (mapped != INVALID_VALUE_REF) {
		return mapped;
	}
	const zend_tpde_value &value =
		clone.callee->values[static_cast<uint32_t>(value_index)];
	if (!value.constant
			|| !zend_mir_scalar_type_is_exact(value.exact_type)
			|| value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
		return INVALID_VALUE_REF;
	}
	mapped = add_derived_value(value.representation,
		value.exact_type, ZEND_MIR_ID_INVALID, true,
		value.constant_bits);
	return mapped;
}

zend_mir_value_id ZendIRAdaptor::source_value_id(
		const zend_mir_source_operand_ref &source) {
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	if (source.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_id =
			zend_mir_value_from_synthetic(source.index);
	} else if ((source.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| source.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& source.ssa_variable_id
				!= ZEND_MIR_ID_INVALID) {
		value_id = zend_mir_value_from_original_ssa(
			source.ssa_variable_id);
	}
	return value_id;
}

IRValueRef ZendIRAdaptor::mapped_source_operand(
		InlineClone &clone, const zend_mir_source_operand_ref &source) {
	const zend_mir_value_id value_id = source_value_id(source);
	return zend_mir_id_is_valid(value_id)
		? mapped_value(clone, value_id) : INVALID_VALUE_REF;
}

int32_t ZendIRAdaptor::checked_step_for_index(
		InlineClone &clone, int32_t value_index) {
	for (uint32_t depth = 0;
			value_index >= 0 && depth < clone.callee->value_count;
			++depth) {
		if (static_cast<uint32_t>(value_index)
				>= clone.checked_value_steps.size()) {
			return -1;
		}
		if (clone.checked_value_steps[
				static_cast<uint32_t>(value_index)] >= 0) {
			return clone.checked_value_steps[
				static_cast<uint32_t>(value_index)];
		}
		const int32_t alias = clone.callee->values[
			static_cast<uint32_t>(value_index)]
			.register_alias_value_index;
		if (alias < 0 || alias == value_index) {
			break;
		}
		value_index = alias;
	}
	return -1;
}

int32_t ZendIRAdaptor::checked_step_for_value(
		InlineClone &clone, zend_mir_value_id value_id) {
	return checked_step_for_index(clone,
		zend_tpde_value_index(clone.callee, value_id));
}

int32_t ZendIRAdaptor::checked_step_for_binding(InlineClone &clone,
		const zend_tpde_source_value_binding &binding) {
	return checked_step_for_index(clone, binding.value_index);
}

void ZendIRAdaptor::record_checked_step(
		InlineClone &clone, int32_t value_index, int32_t step) {
	if (value_index >= 0
			&& static_cast<uint32_t>(value_index)
				< clone.checked_value_steps.size()) {
		clone.checked_value_steps[static_cast<uint32_t>(value_index)] = step;
	}
}

bool ZendIRAdaptor::clone_inline_instruction(
		InlineClone &clone, uint32_t index) {
	const zend_tpde_instruction &instruction =
		clone.callee->instructions[index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(clone.callee, &instruction);
	const zend_tpde_inline_body_step step =
		zend_tpde_inline_body_step_at(
			clone.call, clone.callee, instruction, record, &clone.shape);
	if (step == ZEND_TPDE_INLINE_BODY_REJECT) {
		return false;
	}
	if (step == ZEND_TPDE_INLINE_BODY_SKIP
			|| step == ZEND_TPDE_INLINE_BODY_BRANCH) {
		return true;
	}
	if (step == ZEND_TPDE_INLINE_BODY_PHI) {
		clone.pending_phis.push_back(index);
		return true;
	}
	if (step == ZEND_TPDE_INLINE_BODY_COND_BRANCH) {
		clone.diamond_condition = mapped_value(clone,
			zend_tpde_operand_at(clone.callee, &instruction, 0));
		if (clone.diamond_condition == INVALID_VALUE_REF
				|| exact_type(clone.diamond_condition)
					!= ZEND_MIR_SCALAR_TYPE_I1
				|| machine_kind(clone.diamond_condition)
					!= ZEND_TPDE_MACHINE_VALUE_BOOL) {
			return false;
		}
		return true;
	}
	if (step == ZEND_TPDE_INLINE_BODY_RETURN_SOURCE_ZVAL) {
		return clone_inline_return_source(clone, instruction);
	}
	if (step == ZEND_TPDE_INLINE_BODY_RETURN) {
		const zend_mir_value_id returned_id =
			zend_tpde_operand_at(clone.callee, &instruction, 0);
		if (clone.scalar_diamond) {
			clone.deferred_return_id = returned_id;
		} else {
			clone.returned = mapped_value(clone, returned_id);
			clone.returned_checked_step = clone.returned == INVALID_VALUE_REF
				? checked_step_for_value(clone, returned_id) : -1;
			clone.saw_return = clone.returned != INVALID_VALUE_REF
				|| clone.returned_checked_step >= 0;
		}
		return true;
	}
	if (step == ZEND_TPDE_INLINE_BODY_COPY) {
		const int32_t result_index =
			zend_tpde_value_index(clone.callee, record.result_id);
		const zend_mir_value_id input_id = zend_tpde_operand_at(
			clone.callee, &instruction, 0);
		const IRValueRef input = mapped_value(clone, input_id);
		const int32_t input_checked_step = input == INVALID_VALUE_REF
			? checked_step_for_value(clone, input_id) : -1;
		if (result_index < 0
				|| (input == INVALID_VALUE_REF
					&& input_checked_step < 0)
				|| (input != INVALID_VALUE_REF
					&& exact_type(input)
						!= clone.callee->values[result_index].exact_type)) {
			return false;
		}
		if (input_checked_step >= 0) {
			clone.checked_value_steps[
				static_cast<uint32_t>(result_index)] =
				input_checked_step;
		} else {
			clone.values[static_cast<uint32_t>(result_index)] = input;
		}
		return true;
	}
	if (step == ZEND_TPDE_INLINE_BODY_CHECKED_BINARY) {
		return clone_inline_checked_binary(clone, instruction);
	}
	if (step == ZEND_TPDE_INLINE_BODY_STRING_LENGTH) {
		return clone_inline_string_length(clone, instruction, record);
	}
	/* An effect-free integer or boolean instruction. */
	return clone_inline_scalar(clone, instruction, record);
}

bool ZendIRAdaptor::clone_inline_return_source(
		InlineClone &clone, const zend_tpde_instruction &instruction) {
	zend_mir_value_id returned_id = ZEND_MIR_ID_INVALID;
	const zend_mir_source_operand_ref &source =
		instruction.value_operation.op1;
	if ((source.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				? ((returned_id =
						zend_mir_value_from_synthetic(
							source.index)),
					!zend_mir_id_is_valid(returned_id))
				: ((source.kind
							!= ZEND_MIR_SOURCE_OPERAND_SLOT
						&& source.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| source.ssa_variable_id
						== ZEND_MIR_ID_INVALID
					|| ((returned_id =
							zend_mir_value_from_original_ssa(
								source.ssa_variable_id)),
						!zend_mir_id_is_valid(returned_id))))) {
		return false;
	}
	if (clone.scalar_diamond) {
		clone.deferred_return_id = returned_id;
		clone.deferred_return_binding =
			instruction.source_op1_binding;
		clone.deferred_return_has_binding = true;
	} else {
		clone.returned = mapped_value(clone, returned_id);
		clone.returned_checked_step = clone.returned == INVALID_VALUE_REF
			? checked_step_for_value(clone, returned_id) : -1;
		if (clone.returned_checked_step < 0) {
			clone.returned_checked_step = checked_step_for_binding(clone,
				instruction.source_op1_binding);
		}
		clone.saw_return = clone.returned != INVALID_VALUE_REF
			|| clone.returned_checked_step >= 0;
		if (!clone.saw_return) {
			return false;
		}
	}
	return true;
}

bool ZendIRAdaptor::clone_inline_checked_binary(
		InlineClone &clone, const zend_tpde_instruction &instruction) {
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	zend_mir_value_id result_id = ZEND_MIR_ID_INVALID;
	if (operation.result.kind
			== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		result_id = zend_mir_value_from_synthetic(
			operation.result.index);
	} else if ((operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.result.ssa_variable_id
				!= ZEND_MIR_ID_INVALID) {
		result_id = zend_mir_value_from_original_ssa(
			operation.result.ssa_variable_id);
	}
	if (clone.caller_result == INVALID_VALUE_REF
			|| !zend_mir_id_is_valid(result_id)) {
		return false;
	}
	const int32_t result_index =
		zend_tpde_value_index(clone.callee, result_id);
	const zend_mir_value_id left_id =
		source_value_id(operation.op1);
	const zend_mir_value_id right_id =
		source_value_id(operation.op2);
	const int32_t left_checked_step =
		zend_mir_id_is_valid(left_id)
			? checked_step_for_value(clone, left_id) : -1;
	const int32_t right_checked_step =
		zend_mir_id_is_valid(right_id)
			? checked_step_for_value(clone, right_id) : -1;
	const int32_t bound_left_checked_step =
		left_checked_step >= 0 ? left_checked_step
			: checked_step_for_binding(clone,
				instruction.source_op1_binding);
	const int32_t bound_right_checked_step =
		right_checked_step >= 0 ? right_checked_step
			: checked_step_for_binding(clone,
				instruction.source_op2_binding);
	const IRValueRef left = bound_left_checked_step < 0
		? mapped_source_operand(clone, operation.op1)
		: INVALID_VALUE_REF;
	const IRValueRef right = bound_right_checked_step < 0
		? mapped_source_operand(clone, operation.op2)
		: INVALID_VALUE_REF;
	if (result_index < 0
			|| (bound_left_checked_step < 0
				&& (left == INVALID_VALUE_REF
					|| exact_type(left)
						!= ZEND_MIR_SCALAR_TYPE_I64))
			|| (bound_right_checked_step < 0
				&& (right == INVALID_VALUE_REF
					|| exact_type(right)
						!= ZEND_MIR_SCALAR_TYPE_I64))) {
		return false;
	}
	if (clone.checked_steps.empty()) {
		if (bound_left_checked_step >= 0
				|| bound_right_checked_step >= 0) {
			return false;
		}
		clone.checked_operands.push_back(left);
		clone.checked_operands.push_back(right);
		clone.checked_steps.push_back(
			{operation.source_opcode, true});
	} else {
		const int32_t accumulator_step =
			static_cast<int32_t>(clone.checked_steps.size() - 1);
		const bool accumulator_is_left =
			bound_left_checked_step == accumulator_step
			&& bound_right_checked_step < 0;
		const bool accumulator_is_right =
			bound_right_checked_step == accumulator_step
			&& bound_left_checked_step < 0;
		if (!accumulator_is_left && !accumulator_is_right) {
			return false;
		}
		clone.checked_operands.push_back(
			accumulator_is_left ? right : left);
		clone.checked_steps.push_back({operation.source_opcode,
			accumulator_is_left});
	}
	const int32_t checked_step =
		static_cast<int32_t>(clone.checked_steps.size() - 1);
	record_checked_step(clone, result_index, checked_step);
	record_checked_step(clone,
		instruction.source_result_binding.value_index,
		checked_step);
	return true;
}

bool ZendIRAdaptor::clone_inline_string_length(InlineClone &clone,
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record) {
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	zend_mir_value_id result_id = ZEND_MIR_ID_INVALID;
	if (operation.result.kind
			== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		result_id = zend_mir_value_from_synthetic(
			operation.result.index);
	} else if ((operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.result.ssa_variable_id
				!= ZEND_MIR_ID_INVALID) {
		result_id = zend_mir_value_from_original_ssa(
			operation.result.ssa_variable_id);
	}
	if (!zend_mir_id_is_valid(result_id)) {
		return false;
	}
	const int32_t result_index =
		zend_tpde_value_index(clone.callee, result_id);
	const IRValueRef input =
		mapped_source_operand(clone, operation.op1);
	if (result_index < 0
			|| input == INVALID_VALUE_REF
			|| machine_kind(input)
				!= ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| !machine_value_is_register_authoritative(input)
			|| !machine_value_has_register_definition(input)) {
		return false;
	}
	const uint32_t operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.push_back(input);
	const IRValueRef result = add_derived_value(
		ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_SCALAR_TYPE_I64,
		ZEND_MIR_ID_INVALID, false, 0,
		ZEND_TPDE_MACHINE_VALUE_I64);
	clone.values[static_cast<uint32_t>(result_index)] = result;
	add_node(clone.block_instructions, clone.caller_block, InstNode{
		.kind = InstKind::StringLengthValue,
		.mir_instruction_index = UINT32_MAX,
		.argument_index = UINT32_MAX,
		.result = result,
		.operand_offset = operand_offset,
		.operand_count = 1,
		.has_result = true,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_I64,
		.synthetic = true,
		.synthetic_record = record});
	clone.cloned_string_length = true;
	return true;
}

bool ZendIRAdaptor::clone_inline_scalar(InlineClone &clone,
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record) {
	const int32_t result_index =
		zend_tpde_value_index(clone.callee, record.result_id);
	if (result_index < 0
			|| !zend_mir_scalar_type_is_exact(
				clone.callee->values[result_index].exact_type)
			|| clone.callee->values[result_index].exact_type
				== ZEND_MIR_SCALAR_TYPE_NULL
			|| clone.callee->values[result_index].exact_type
				== ZEND_MIR_SCALAR_TYPE_F64) {
		return false;
	}
	const uint32_t operand_offset =
		static_cast<uint32_t>(operands_.size());
	for (uint32_t operand_index = 0;
			operand_index < instruction.operand_count;
			++operand_index) {
		const IRValueRef operand = mapped_value(clone,
			zend_tpde_operand_at(
				clone.callee, &instruction, operand_index));
		if (operand == INVALID_VALUE_REF) {
			return false;
		}
		operands_.push_back(operand);
	}
	const IRValueRef result = add_derived_value(
		clone.callee->values[result_index].representation,
		clone.callee->values[result_index].exact_type,
		ZEND_MIR_ID_INVALID);
	clone.values[static_cast<uint32_t>(result_index)] = result;
	add_node(clone.block_instructions, clone.caller_block, InstNode{
		.kind = InstKind::MIR,
		.mir_instruction_index = UINT32_MAX,
		.argument_index = UINT32_MAX,
		.result = result,
		.operand_offset = operand_offset,
		.operand_count = instruction.operand_count,
		.has_result = true,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.synthetic = true,
		.synthetic_record = record});
	return true;
}

bool ZendIRAdaptor::select_inline_diamond(InlineClone &clone) {
	if (clone.diamond_condition == INVALID_VALUE_REF
			|| clone.pending_phis.empty()) {
		return false;
	}
	const uint32_t predecessor_begin =
		clone.callee->block_predecessor_offsets[clone.diamond.merge];
	for (uint32_t instruction_index : clone.pending_phis) {
		const zend_tpde_instruction &instruction =
			clone.callee->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(clone.callee, &instruction);
		const int32_t result_index =
			zend_tpde_value_index(clone.callee, record.result_id);
		if (result_index < 0 || instruction.operand_count != 2) {
			return false;
		}
		IRValueRef true_value = INVALID_VALUE_REF;
		IRValueRef false_value = INVALID_VALUE_REF;
		for (uint32_t operand = 0; operand < 2; ++operand) {
			const uint32_t predecessor = clone.callee->block_predecessors[
				predecessor_begin + operand];
			const IRValueRef input = mapped_value(clone,
				zend_tpde_operand_at(clone.callee, &instruction, operand));
			if (input == INVALID_VALUE_REF) {
				return false;
			}
			if (predecessor == clone.diamond.true_block) {
				true_value = input;
			} else if (predecessor == clone.diamond.false_block) {
				false_value = input;
			} else {
				return false;
			}
		}
		const zend_tpde_value &callee_result =
			clone.callee->values[static_cast<uint32_t>(result_index)];
		if (true_value == INVALID_VALUE_REF
				|| false_value == INVALID_VALUE_REF
				|| exact_type(true_value) != callee_result.exact_type
				|| exact_type(false_value) != callee_result.exact_type
				|| representation(true_value)
					!= callee_result.representation
				|| representation(false_value)
					!= callee_result.representation
				|| machine_kind(true_value) != callee_result.machine_kind
				|| machine_kind(false_value)
					!= callee_result.machine_kind
				|| !zend_tpde_machine_kind_is_long_or_bool(
					callee_result.machine_kind)) {
			return false;
		}
		const IRValueRef result = add_derived_value(
			callee_result.representation, callee_result.exact_type,
			ZEND_MIR_ID_INVALID, false, 0,
			callee_result.machine_kind);
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(clone.diamond_condition);
		operands_.push_back(true_value);
		operands_.push_back(false_value);
		clone.values[static_cast<uint32_t>(result_index)] = result;
		add_node(clone.block_instructions, clone.caller_block, InstNode{
			.kind = InstKind::ScalarSelect,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = result,
			.operand_offset = operand_offset,
			.operand_count = 3,
			.has_result = true,
			.storage_id = ZEND_MIR_ID_INVALID,
			.exact_type = callee_result.exact_type,
			.synthetic = true,
			.synthetic_record = record});
	}
	clone.returned = mapped_value(clone, clone.deferred_return_id);
	if (clone.returned == INVALID_VALUE_REF
			&& clone.deferred_return_has_binding
			&& clone.deferred_return_binding.value_index >= 0
			&& static_cast<uint32_t>(
				clone.deferred_return_binding.value_index)
				< clone.callee->value_count) {
		clone.returned = mapped_value(clone,
			clone.callee->values[static_cast<uint32_t>(
				clone.deferred_return_binding.value_index)].id);
	}
	return true;
}

ZendIRAdaptor::ZendIRAdaptor(Unbuilt, const zend_tpde_plan *plan,
		std::span<const zend_tpde_plan *const> component_plans,
		FunctionMode function_mode)
	: plan_(plan), component_plans_(component_plans),
	  function_mode_(function_mode) {}

class ZendIRAdaptor::Builder : public ZendIRAdaptor {
public:
	Builder(const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			FunctionMode function_mode)
		: ZendIRAdaptor(Unbuilt{}, plan, component_plans, function_mode),
		  machine_cfg(function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_machine_cfg
			: plan_->entry_machine_cfg),
		  boxed_cond_cold_blocks(machine_cfg.boxed_cond_cold_blocks),
		  boxed_cond_cold_by_predecessor(
			machine_cfg.boxed_cond_cold_by_predecessor),
		  instruction_blocks(machine_cfg.instruction_blocks),
		  guarded_cold_blocks(machine_cfg.guarded_cold_blocks),
		  guarded_hot_blocks(machine_cfg.guarded_hot_blocks),
		  guarded_continuation_blocks(
			machine_cfg.guarded_continuation_blocks),
		  final_blocks(machine_cfg.final_blocks),
		  tpde_block_count(machine_cfg.block_count),
		  machine_value_used{function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_value_required
			: plan_->entry_value_required} {}

	/* Builds the graph and hands over the adaptor part. */
	ZendIRAdaptor &&build() {
		run();
		return std::move(*this);
	}

private:
	/*
	 * The frozen uses of this function's mode
	 * (machine_value_has_frozen_use). A call publishes its canonical
	 * result zval and needs a register result besides only when a TPDE
	 * instruction or PHI consumes it.
	 */
	struct FrozenUses {
		const uint8_t *required;
		uint8_t operator[](uint32_t value) const {
			return required != nullptr && required[value] != 0;
		}
	};

	struct RegisterAssignmentPhiSource {
		zend_mir_storage_id storage_id;
		IRValueRef value;
	};

	struct PendingPhiInput {
		InstKind kind;
		uint32_t block;
		IRValueRef source;
		IRValueRef result;
		zend_mir_storage_id storage_id;
		zend_mir_scalar_type_mask exact_type;
		bool emitted = false;
	};

	/* The machine CFG of this function's mode. */
	const zend_tpde_machine_cfg &machine_cfg;
	const uint32_t *const boxed_cond_cold_blocks;
	const uint32_t *const boxed_cond_cold_by_predecessor;
	const uint32_t *const instruction_blocks;
	const uint32_t *const guarded_cold_blocks;
	const uint32_t *const guarded_hot_blocks;
	const uint32_t *const guarded_continuation_blocks;
	const uint32_t *const final_blocks;
	const uint32_t tpde_block_count;
	const FrozenUses machine_value_used;

	/* The state the construction phases below share. */
	std::vector<BlockItem<IRInstRef>> block_instructions;
	std::vector<BlockItem<IRValueRef>> block_phis;
	std::vector<uint8_t> generator_resume_emitted;
	std::vector<uint8_t> source_landing_emitted;
	std::vector<uint32_t> source_landing_blocks;
	/* The entry block lands no source opcode (see map_source_landings). */
	bool source_synthetic_entry = false;
	std::vector<uint32_t> source_block_next;
	bool source_call_fragments = false;
	int32_t entry = -1;
	bool source_landings = false;
	std::vector<IRValueRef> transient_scalar_results;
	std::vector<IRValueRef> register_component_results;
	std::vector<IRValueRef> preselected_phi_results;
	std::vector<IRValueRef> register_assignment_sources;
	std::vector<IRValueRef> register_assignment_results;
	std::vector<RegisterAssignmentPhiSource> register_assignment_phi_sources;
	std::vector<uint8_t> source_result_used;
	std::vector<int32_t> source_result_consumer;
	std::vector<uint8_t> source_result_materialized_for_helper;
	std::vector<int32_t> source_producer_by_value;
	std::vector<IRValueRef> register_binary_results;
	std::vector<IRValueRef> register_branch_results;
	std::vector<IRValueRef> register_boolean_results;
	std::vector<zend_mir_storage_id> register_boxed_storages;
	std::vector<IRValueRef> mutation_results;
	std::vector<zend_mir_storage_id> lazy_mutation_storages;
	std::vector<zend_mir_storage_id> frame_authoritative_phi_storages;
	std::vector<std::pair<uint32_t, zend_mir_value_id>>
		deferred_typed_phi_inputs;
	std::vector<IRValueRef> boxed_phi_input_overrides;
	std::vector<IRValueRef> boxed_phi_cold_input_overrides;
	std::vector<PendingPhiInput> pending_phi_inputs;
	std::vector<IRValueRef> selected_phi_results;
	std::vector<uint8_t> runtime_short_circuit_result_values;
	ResultDefinitions selected_phi_definitions;
	ResultDefinitions register_assignment_definitions;
	/*
	 * Instructions a Zend entry emits as the register definition of their
	 * own MIR result without listing it among the instruction results: a
	 * scalar operation with a machine result and a selected machine COPY.
	 * A boxed-identity COPY of such a value reads that register.
	 */
	std::vector<uint8_t> entry_register_definitions;
	std::vector<uint32_t> resumes_by_target;
	std::vector<uint32_t> resumes_by_landing;
	/* Per IR value, 1 + the last resume that listed it. */
	std::vector<uint32_t> resume_value_stamps;
	/* Per source position, the first instruction (in index order) whose
	 * user-opcode call fragment runs there. */
	std::vector<uint32_t> call_fragment_instructions;
	std::vector<uint32_t> pending_by_block;
	std::vector<uint8_t> elided_silence_instructions;
	std::vector<int32_t> latest_source_producer_by_ir_value;
	/*
	 * Per storage, in index order, the instructions that write its slot
	 * as their op1 and so may leave a register stale for a later return
	 * of that slot. Built on the first query.
	 */
	std::unordered_map<zend_mir_storage_id, std::vector<uint32_t>>
		slot_writers;
	bool slot_writers_built = false;
	const std::vector<uint32_t> no_slot_writers;

	/*
	 * The instruction build_instruction() lowers and the state its lowering
	 * steps share. Each instruction gets a fresh context.
	 */
	struct InstContext {
		const uint32_t i;
		const zend_tpde_instruction &instruction;
		const zend_mir_instruction_record record;
		const bool boxed_cond_branch;
		const uint32_t block;
		IRValueRef result = INVALID_VALUE_REF;
		ScalarTypeCheckSelection type_check_selection =
			ScalarTypeCheckSelection::Invalid;
		IRValueRef type_check_input = INVALID_VALUE_REF;
		IRValueRef register_condition = INVALID_VALUE_REF;
		bool register_cond_branch = false;
		zend_mir_scalar_type_mask canonical_bool_unary_exact_type =
			ZEND_MIR_SCALAR_TYPE_NONE;
		IRValueRef register_bool_unary_operand = INVALID_VALUE_REF;
		IRValueRef register_boxed_condition_operand = INVALID_VALUE_REF;
		IRValueRef register_string_condition_operand = INVALID_VALUE_REF;
		IRValueRef register_string_length_operand = INVALID_VALUE_REF;
		bool constant_string_length = false;
		uint64_t constant_string_length_bits = 0;
		bool register_component_call = false;
		IRValueRef copy_input = INVALID_VALUE_REF;
		bool machine_result = false;
		InlinedBody inlined_user_body;
		uint32_t operand_offset = 0;
		std::vector<std::pair<IRValueRef, uint32_t>> typed_call_value_guards;
		std::vector<IRValueRef> typed_call_owned_boxed_arguments;
		uint32_t assign_op_right_operand_index = UINT32_MAX;
		uint32_t assign_op_left_operand_index = UINT32_MAX;
		uint32_t packed_append_value_operand_index = UINT32_MAX;
		uint32_t property_write_value_operand_index = UINT32_MAX;
		bool static_slot_isset_needs_value = false;
		zend_mir_scalar_type_mask static_slot_isset_exact_type =
			ZEND_MIR_SCALAR_TYPE_NONE;
		bool static_slot_isset_machine_fast = false;
		IRValueRef static_slot_isset_operand = INVALID_VALUE_REF;
		bool explicit_long_operands = false;
		uint32_t machine_reference_operand_index = UINT32_MAX;
		uint32_t direct_call_context_operand = UINT32_MAX;
		bool direct_internal_argument_transport = false;
		uint32_t direct_internal_boxed_argument_count = 0;
		bool lazy_store = false;
		uint32_t data_operand_count = 0;
		uint32_t operand_count = 0;
		uint32_t inlined_operand_index = UINT32_MAX;
		uint32_t materialization_count = 0;
		uint32_t materialization_operand_index = UINT32_MAX;
		uint32_t boxed_cond_cold_block = UINT32_MAX;
		uint32_t guarded_cold_block = UINT32_MAX;
		IRValueRef boxed_op1_boundary = INVALID_VALUE_REF;
		IRValueRef boxed_op2_boundary = INVALID_VALUE_REF;
		uint32_t boxed_op1_boundary_operand_index = UINT32_MAX;
		uint32_t boxed_op2_boundary_operand_index = UINT32_MAX;
		uint32_t boundary_semantic_operand_count = UINT32_MAX;
		/* The node emit_instruction_node() added for the instruction;
		 * later steps may add nodes after it. */
		uint32_t main_node = UINT32_MAX;
	};
	/*
	 * The fast and the cold node lower_guarded_operation() splits an
	 * instruction into, which its steps share.
	 */
	struct GuardedSplit {
		uint32_t guarded_hot_block = UINT32_MAX;
		uint32_t continuation_block = UINT32_MAX;
		bool frame_authoritative_mutation = false;
		IRValueRef mutation_result = INVALID_VALUE_REF;
		bool register_mutation_result = false;
		IRValueRef guarded_result = INVALID_VALUE_REF;
		bool deopt_exit = false;
		IRValueRef fast_result = INVALID_VALUE_REF;
		IRValueRef cold_result = INVALID_VALUE_REF;
		uint32_t fast_operand_offset = 0;
		uint32_t fast_operand_count = 0;
		uint32_t fast_materialization_operand_index = UINT32_MAX;
		uint32_t fast_materialization_count = 0;
		uint32_t fast_block = UINT32_MAX;
		bool value_assign_source_operand = false;
		uint32_t cold_operand_offset = 0;
		uint32_t cold_operand_count = 0;
		uint32_t cold_materialization_operand_index = UINT32_MAX;
	};

	void run();

	bool init_value_tables();
	bool build_blocks();
	void map_source_landings();
	void add_entry_nodes();
	void load_register_arguments();
	void select_component_results();
	void preselect_pointer_phis();
	void select_register_assignments();
	void mark_source_result_consumers();
	void select_register_boolean_results();
	void collect_call_transport_storages();
	void select_register_binary_results();
	void select_register_branch_results();
	void select_packed_fetch_results();
	void close_register_boxed_storages();
	void reselect_register_assignments();
	void select_mutation_results();
	void select_phi_results();
	void resolve_phi_edges();
	void load_slot_backed_phi_inputs();
	void merge_short_circuit_results();
	void order_generator_resumes();
	void index_call_fragments();
	void index_pending_phi_inputs();
	void elide_empty_silence_pairs();
	void build_instructions();
	void build_instruction(uint32_t i);
	/* The steps of build_instruction(), in order. A step returning false
	 * ends the instruction. */
	void emit_instruction_landings(InstContext &ctx);
	void select_initial_result(InstContext &ctx);
	void select_register_operands(InstContext &ctx);
	bool select_component_call_result(InstContext &ctx);
	void select_binary_result(InstContext &ctx);
	bool select_phi_result(InstContext &ctx);
	void select_source_scalar_result(InstContext &ctx);
	bool select_source_boxed_result(InstContext &ctx);
	void select_internal_call_result(InstContext &ctx);
	bool select_machine_result(InstContext &ctx);
	bool lower_boxed_copy(InstContext &ctx);
	void add_value_assign_operands(InstContext &ctx);
	void add_fetch_operands(InstContext &ctx);
	void add_compound_assign_operands(InstContext &ctx);
	void add_write_value_operands(InstContext &ctx);
	void add_register_unary_operands(InstContext &ctx);
	void add_slot_isset_operand(InstContext &ctx);
	void add_long_binary_operands(InstContext &ctx);
	void add_literal_reference_operand(InstContext &ctx);
	void add_data_operands(InstContext &ctx);
	void add_frame_and_call_operands(InstContext &ctx);
	void count_operands(InstContext &ctx);
	void add_boxed_boundary_operands(InstContext &ctx);
	void emit_instruction_node(InstContext &ctx);
	void publish_init_array_pointer(InstContext &ctx);
	void mark_selected_operation(InstContext &ctx);
	void set_result_flag(uint32_t &flags, uint32_t flag, bool required);
	bool transport_canonical_long(InstContext &ctx, IRValueRef &value);
	IRValueRef direct_boxed_source(InstContext &ctx,
			const zend_tpde_source_value_binding &binding,
			const zend_mir_source_operand_ref &source,
			zend_mir_storage_id storage,
			uint32_t definition_ssa_variable_id_plus_one);
	void lower_typed_body_operation(
			uint32_t i, const zend_tpde_instruction &instruction,
			uint32_t block);
	void forward_typed_body_assign(
			uint32_t i, const zend_tpde_instruction &instruction,
			uint32_t block);
	void lower_phi(InstContext &ctx);
	void append_data_operands(InstContext &ctx);
	void add_return_source_operands(
			uint32_t i, const zend_tpde_instruction &instruction);
	void add_user_call_operands(InstContext &ctx);
	void add_typed_call_operands(InstContext &ctx);
	void add_typed_call_argument(InstContext &ctx, uint32_t n,
			std::vector<IRValueRef> &typed_call_values);
	void add_zend_frame_call_operands(InstContext &ctx);
	void add_internal_call_operands(InstContext &ctx);
	void add_materialization_operands(InstContext &ctx);
	uint32_t append_boundary_operand(
			IRValueRef candidate, uint32_t segment_offset,
			uint32_t semantic_count, uint32_t &segment_count);
	void reload_slot_authoritative_source_result(
			uint32_t i, const zend_tpde_instruction &instruction,
			const zend_mir_instruction_record &record, uint32_t reload_block);
	void materialize_boxed_result_for_helper(
			uint32_t i, uint32_t materialization_block, IRValueRef value);
	void lower_guarded_operation(InstContext &ctx);
	/* The steps of lower_guarded_operation(), in order. */
	void select_guarded_mutation_result(InstContext &ctx, GuardedSplit &split);
	void select_guarded_results(InstContext &ctx, GuardedSplit &split);
	void select_fast_operands(InstContext &ctx, GuardedSplit &split);
	void add_typed_call_guard(InstContext &ctx, GuardedSplit &split);
	void add_guarded_fast_node(InstContext &ctx, GuardedSplit &split);
	void select_cold_operands(InstContext &ctx, GuardedSplit &split);
	void add_guarded_cold_node(InstContext &ctx, GuardedSplit &split);
	void join_guarded_results(InstContext &ctx, GuardedSplit &split);
	void publish_assigned_pointer(InstContext &ctx, GuardedSplit &split);
	void lower_boxed_cond_branch(InstContext &ctx);
	void verify_emission();
	void index_landings();
	void attach_generator_resume_values();
	void emit_trailing_source_landings();
	void prune_unused_argument_loads();
	void resolve_deferred_typed_phi_inputs();
	void finalize_graph();

	/* Whether the entry reaches block. */
	bool machine_block_reachable(uint32_t block) const;
	bool machine_block_dominates(uint32_t dominator, uint32_t block);
	bool phased_source_call(uint32_t instruction_index);
	void select_register_assignment(uint32_t i);
	IRValueRef register_assignment_phi_source(zend_mir_storage_id storage_id);
	void mark_producer(int32_t producer, uint32_t consumer_index);
	void mark_binding(const zend_tpde_source_value_binding &binding,
			uint32_t consumer_index);
	bool source_result_has_direct_consumer(uint32_t producer_index);
	void select_mutation_result(uint32_t i);
	bool boxed_phi_needs_scalar_transport(
			const zend_tpde_instruction &instruction,
			zend_mir_storage_id result_storage);
	bool phi_uses_runtime_short_circuit_slot(
			const zend_tpde_instruction &instruction,
			zend_mir_storage_id result_storage);
	IRValueRef value_ref_for_block(
			zend_mir_value_id value_id, uint32_t use_block,
			uint32_t use_instruction);
	bool register_operand_reaches(
			IRValueRef value, uint32_t use_block, uint32_t use_instruction);
	bool entry_argument_has_register_definition(IRValueRef value);
	IRValueRef resolve_materializable_scalar(
			uint32_t value_index, uint32_t use_block, uint32_t use_instruction,
			uint32_t depth);
	bool entry_register_definition(IRValueRef value);
	IRValueRef resolve_copy_input(
			zend_mir_value_id value_id, uint32_t use_block,
			uint32_t use_instruction);
	IRValueRef phi_edge_value_ref(
			zend_mir_value_id value_id, uint32_t predecessor_block);
	void emit_generator_resume(uint32_t block, uint32_t source_position,
			bool exact_source_position, uint32_t use_instruction);
	void emit_source_landing(
			uint32_t block, uint32_t source_position, uint32_t use_instruction);
	std::ranges::subrange<std::vector<uint32_t>::iterator> block_pending(
			uint32_t block);
	void emit_pending_phi_inputs(
			uint32_t block, uint32_t terminator_instruction);
	bool has_pending_phi_inputs(uint32_t block);
	void record_source_producer(uint32_t instruction_index);
	const std::vector<uint32_t> &canonical_slot_writers(
			zend_mir_storage_id storage);
};

ZendIRAdaptor::ZendIRAdaptor(const zend_tpde_plan *plan,
		std::span<const zend_tpde_plan *const> component_plans,
		FunctionMode function_mode)
	: ZendIRAdaptor(Builder(plan, component_plans, function_mode).build()) {}

void ZendIRAdaptor::Builder::run() {
	if (!init_value_tables()) {
		return;
	}
	if (!build_blocks()) {
		return;
	}
	map_source_landings();
	add_entry_nodes();
	load_register_arguments();
	select_component_results();
	preselect_pointer_phis();
	select_register_assignments();
	mark_source_result_consumers();
	select_register_boolean_results();
	collect_call_transport_storages();
	select_register_binary_results();
	select_register_branch_results();
	select_packed_fetch_results();
	close_register_boxed_storages();
	reselect_register_assignments();
	select_mutation_results();
	select_phi_results();
	resolve_phi_edges();
	load_slot_backed_phi_inputs();
	merge_short_circuit_results();
	order_generator_resumes();
	index_call_fragments();
	index_pending_phi_inputs();
	elide_empty_silence_pairs();
	build_instructions();
	verify_emission();
	index_landings();
	attach_generator_resume_values();
	emit_trailing_source_landings();
	prune_unused_argument_loads();
	resolve_deferred_typed_phi_inputs();
	finalize_graph();
}

bool ZendIRAdaptor::Builder::init_value_tables() {
	value_overrides_.assign(plan_->value_count, INVALID_VALUE_REF);
	source_ssa_overrides_.assign(
		plan_->source_ssa_variable_count, INVALID_VALUE_REF);
	instruction_results_.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	for (uint32_t index = 0;
			index < plan_->machine_reference_count; ++index) {
		const zend_tpde_machine_reference &reference =
			plan_->machine_references[index];
		if (reference.kind != ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT
				|| reference.stable_storage_or_layout_id
					> plan_->machine_reference_count + 4096) {
			continue;
		}
		if (reference.stable_storage_or_layout_id
				>= frame_slot_references_.size()) {
			frame_slot_references_.resize(
				reference.stable_storage_or_layout_id + 1, UINT32_MAX);
		}
		if (frame_slot_references_[
				reference.stable_storage_or_layout_id] == UINT32_MAX) {
			frame_slot_references_[
				reference.stable_storage_or_layout_id] = index;
		}
	}
	zend_tpde_collect_reference_assigned_storages(
		plan_, reference_assigned_storages_);
	if (function_mode_ == FunctionMode::ZendEntry) {
		arguments_.push_back(IRValueRef{EXECUTE_DATA_VALUE});
		arguments_.push_back(
			IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
	} else {
		if (!plan_->typed_body_eligible
				|| !plan_->typed_body_return_abi.valid) {
			valid_ = false;
			return false;
		}
		arguments_.reserve(plan_->argument_count);
		for (uint32_t argument = 0;
				argument < plan_->argument_count; ++argument) {
			arguments_.push_back(IRValueRef{
				MIR_VALUE_BASE + static_cast<uint32_t>(
					plan_->argument_value_indices[argument])});
		}
		for (uint32_t value = 0;
				value < plan_->value_count; ++value) {
			const int32_t alias =
				plan_->values[value].register_alias_value_index;
			if (valid_value_index(alias)) {
				value_overrides_[value] = IRValueRef{
					MIR_VALUE_BASE + static_cast<uint32_t>(alias)};
			}
		}
	}
	return true;
}

bool ZendIRAdaptor::Builder::build_blocks() {
	blocks_.reserve(plan_->block_count);
	block_instructions.reserve(plan_->instruction_count + plan_->value_count + 1);
	block_phis.reserve(plan_->value_count);
	phi_input_slices_.resize(MIR_VALUE_BASE + plan_->value_count);
	phi_values_.resize(MIR_VALUE_BASE + plan_->value_count);
	if (tpde_block_count < plan_->block_count
			|| machine_cfg.successor_offsets == nullptr
			|| (plan_->instruction_count != 0
				&& (instruction_blocks == nullptr
					|| guarded_cold_blocks == nullptr
					|| guarded_hot_blocks == nullptr
					|| guarded_continuation_blocks == nullptr
					|| boxed_cond_cold_blocks == nullptr))
			|| (plan_->block_count != 0
				&& (final_blocks == nullptr
					|| boxed_cond_cold_by_predecessor == nullptr))) {
		valid_ = false;
		return false;
	}
	blocks_.reserve(tpde_block_count);
	block_info_.resize(tpde_block_count);
	block_info2_.resize(tpde_block_count);
	for (uint32_t i = 0; i < tpde_block_count; ++i) {
		blocks_.push_back(IRBlockRef{i});
	}
	successor_slices_.resize(tpde_block_count);
	successors_.reserve(machine_cfg.successor_count);
	for (uint32_t block = 0; block < tpde_block_count; ++block) {
		const uint32_t begin =
			machine_cfg.successor_offsets[block];
		const uint32_t end =
			machine_cfg.successor_offsets[block + 1];
		if (begin > end || end > machine_cfg.successor_count) {
			valid_ = false;
			return false;
		}
		successor_slices_[block] = {begin, end - begin};
	}
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		if (instruction_blocks[i] >= tpde_block_count
				|| (guarded_cold_blocks[i] != UINT32_MAX
					&& guarded_cold_blocks[i] >= tpde_block_count)
				|| (guarded_hot_blocks[i] != UINT32_MAX
					&& guarded_hot_blocks[i] >= tpde_block_count)
				|| (guarded_continuation_blocks[i] != UINT32_MAX
					&& guarded_continuation_blocks[i]
						>= tpde_block_count)
				|| (boxed_cond_cold_blocks[i] != UINT32_MAX
					&& boxed_cond_cold_blocks[i]
						>= tpde_block_count)) {
			valid_ = false;
			return false;
		}
	}
	for (uint32_t block = 0; block < plan_->block_count; ++block) {
		if (final_blocks[block] >= tpde_block_count
				|| (boxed_cond_cold_by_predecessor[block]
						!= UINT32_MAX
					&& boxed_cond_cold_by_predecessor[block]
						>= tpde_block_count)) {
			valid_ = false;
			return false;
		}
	}
	entry = block_index(plan_->function.entry_block_id);
	if (entry < 0) {
		valid_ = false;
		return false;
	}
	for (uint32_t edge = 0;
			edge < machine_cfg.successor_count; ++edge) {
		if (machine_cfg.successors == nullptr
				|| machine_cfg.successors[edge]
					>= tpde_block_count) {
			valid_ = false;
			return false;
		}
		successors_.push_back(
			IRBlockRef{machine_cfg.successors[edge]});
	}
	return true;
}

bool ZendIRAdaptor::Builder::machine_block_reachable(uint32_t block) const {
	return machine_cfg.dominators.roots[block]
		== static_cast<uint32_t>(entry);
}

bool ZendIRAdaptor::Builder::machine_block_dominates(
		uint32_t dominator, uint32_t block) {
	if (dominator >= tpde_block_count || block >= tpde_block_count) {
		return false;
	}
	if (dominator == block) {
		return true;
	}
	return zend_mir_dominator_forest_dominates(
		&machine_cfg.dominators, dominator, block);
}

bool ZendIRAdaptor::Builder::phased_source_call(uint32_t instruction_index) {
	if (function_mode_ != FunctionMode::ZendEntry
			|| plan_->source_call_phase_count == 0
			|| instruction_index >= plan_->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &instruction =
		plan_->instructions[instruction_index];
	const zend_mir_instruction_record record =
		instruction_record_at(instruction_index);
	return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& instruction.user_call != nullptr
		&& instruction.direct_call == nullptr
		&& instruction.user_call->do_opcode != ZEND_CALLABLE_CONVERT
		&& instruction.user_call->do_opcode
			!= ZEND_CALLABLE_CONVERT_PARTIAL;
}

void ZendIRAdaptor::Builder::map_source_landings() {
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan_->instructions[i];
		source_call_fragments =
			source_call_fragments
			|| (instruction.user_opcode_call_fragments
				&& !phased_source_call(i));
	}
	generator_resume_emitted.resize(
		plan_->generator_resume_count, 0);
	source_landings =
		plan_->user_opcode_callbacks || source_call_fragments
			|| (function_mode_ == FunctionMode::ZendEntry
				&& plan_->source_call_phase_count != 0);
	if (source_landings && plan_->source_opcodes != nullptr) {
		source_landing_emitted.resize(plan_->source_opcode_count, 0);
		source_landing_blocks.resize(
			plan_->source_opcode_count, UINT32_MAX);
		if (plan_->user_opcode_callbacks) {
			user_opcode_next_landings_.resize(
				plan_->source_opcode_count, UINT32_MAX);
			user_opcode_result_reload_sources_.resize(
				plan_->source_opcode_count, 0);
		}
		if (plan_->source_opcode_block_indices == nullptr
				|| plan_->source_opcode_is_data == nullptr
				|| plan_->source_block_starts == nullptr
				|| plan_->source_block_ends == nullptr) {
			valid_ = false;
		} else {
			source_block_next.resize(
				plan_->source_block_count, UINT32_MAX);
			/*
			 * A loop back to the first opcode gets an entry block of its
			 * own, whose arguments and constants name source position 0
			 * while the opcode runs at the loop header: such an entry
			 * lands no source opcode.
			 */
			bool synthetic_entry = plan_->source_opcode_count != 0
				&& plan_->source_opcodes[0].opcode != ZEND_RECV
				&& plan_->source_opcodes[0].opcode != ZEND_RECV_INIT
				&& plan_->source_opcodes[0].opcode != ZEND_RECV_VARIADIC;
			bool first_block_after_entry = false;
			for (uint32_t instruction = 0; synthetic_entry
					&& instruction < plan_->instruction_count;
					++instruction) {
				const zend_mir_instruction_record record =
					instruction_record_at(instruction);
				if (record.source_position_id
						>= plan_->source_opcode_count) {
					continue;
				}
				if (record.block_id == plan_->function.entry_block_id) {
					synthetic_entry = record.source_position_id == 0;
				} else if (plan_->source_opcode_block_indices[
							record.source_position_id]
						== plan_->source_opcode_block_indices[0]) {
					first_block_after_entry = true;
				}
			}
			synthetic_entry = synthetic_entry && first_block_after_entry;
			source_synthetic_entry = synthetic_entry;
			for (uint32_t instruction = 0;
					instruction < plan_->instruction_count;
					++instruction) {
				const zend_mir_instruction_record record =
					instruction_record_at(instruction);
				if (record.source_position_id
						>= plan_->source_opcode_count
						|| (synthetic_entry
							&& record.block_id
								== plan_->function.entry_block_id)) {
					continue;
				}
				const uint32_t source_block =
					plan_->source_opcode_block_indices[
						record.source_position_id];
				const uint32_t mir_block =
					instruction_blocks[instruction];
				if (source_block >= plan_->source_block_count
						|| mir_block == UINT32_MAX) {
					valid_ = false;
					continue;
				}
				/*
				 * Lowering may split one Zend basic block into multiple MIR
				 * blocks.  Keep the first executable landing for each exact
				 * source position; gaps are assigned to the next executable
				 * position below so call INIT/SEND fragments run before the
				 * operation that consumes their state.
				 */
				if (source_landing_blocks[record.source_position_id]
						== UINT32_MAX) {
					source_landing_blocks[record.source_position_id] =
						mir_block;
				}
			}
			for (uint32_t source_block = 0;
					source_block < plan_->source_block_count;
					++source_block) {
				const uint32_t block_start =
					plan_->source_block_starts[source_block];
				const uint32_t block_end =
					plan_->source_block_ends[source_block];
				if (block_start == UINT32_MAX
						|| block_end == UINT32_MAX) {
					continue;
				}
				source_block_next[source_block] = block_start;
				uint32_t next_mir_block = UINT32_MAX;
				for (uint32_t source = block_end;
						source-- > block_start;) {
					if (plan_->source_opcode_is_data[source] != 0) {
						continue;
					}
					if (source_landing_blocks[source] != UINT32_MAX) {
						next_mir_block = source_landing_blocks[source];
					} else if (next_mir_block != UINT32_MAX) {
						source_landing_blocks[source] = next_mir_block;
					}
				}
				uint32_t previous_mir_block = UINT32_MAX;
				for (uint32_t source = block_start;
						source < block_end; ++source) {
					if (plan_->source_opcode_is_data[source] != 0) {
						continue;
					}
					if (source_landing_blocks[source] != UINT32_MAX) {
						previous_mir_block =
							source_landing_blocks[source];
					} else if (previous_mir_block != UINT32_MAX) {
						source_landing_blocks[source] =
							previous_mir_block;
					} else {
						valid_ = false;
					}
				}
			}
		}
		if (plan_->user_opcode_callbacks) {
			uint32_t next = UINT32_MAX;
			for (uint32_t source = plan_->source_opcode_count;
					source-- > 0;) {
				if (source_landing_blocks[source] != UINT32_MAX) {
					next = source;
				}
				user_opcode_next_landings_[source] = next;
			}
			for (uint32_t source = 0;
					source < plan_->source_opcode_count; ++source) {
				if (source_landing_blocks[source] == UINT32_MAX
						|| source
							>= plan_->user_opcode_source_operation_count) {
					continue;
				}
				user_opcode_dispatch_to_sources_.push_back(source);
			}
		}
	}
}

void ZendIRAdaptor::Builder::add_entry_nodes() {
	if (function_mode_ == FunctionMode::ZendEntry) {
		operands_.push_back(IRValueRef{EXECUTE_DATA_VALUE});
		/* A deoptimization entry keeps the frame's temporaries: the
		 * frame load reads the context's deopt_resume. */
		if (plan_->deopt_resume_count != 0
				|| plan_->tier2_fallback_entry != nullptr) {
			operands_.push_back(IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		}
		add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
			.kind = InstKind::LoadFrame,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = IRValueRef{FRAME_VALUE},
			.operand_offset = 0,
			.operand_count = static_cast<uint32_t>(operands_.size()),
			.has_result = true});
		if (plan_->generator_resume_count != 0) {
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
			add_node(block_instructions, static_cast<uint32_t>(entry),
				InstNode{
					.kind = InstKind::GeneratorGateway,
					.mir_instruction_index = UINT32_MAX,
					.argument_index = UINT32_MAX,
					.result = INVALID_VALUE_REF,
					.operand_offset = operand_offset,
					.operand_count = 2,
					.has_result = false});
		}
		if (plan_->deopt_resume_count != 0) {
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
			InstNode gateway{
				.kind = InstKind::GeneratorGateway,
				.mir_instruction_index = UINT32_MAX,
				.argument_index = UINT32_MAX,
				.result = INVALID_VALUE_REF,
				.operand_offset = operand_offset,
				.operand_count = 2,
				.has_result = false};
			gateway.deopt_resume = true;
			add_node(block_instructions, static_cast<uint32_t>(entry),
				std::move(gateway));
		}
		uint32_t guard_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(IRValueRef{FRAME_VALUE});
		add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
			.kind = InstKind::ZvalGuardArguments,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = UINT32_MAX,
			.result = INVALID_VALUE_REF,
			.operand_offset = guard_operand_offset,
			.operand_count = 1,
			.has_result = false});
	}
}

void ZendIRAdaptor::Builder::load_register_arguments() {
	for (uint32_t i = 0; i < plan_->value_count; ++i) {
		const zend_tpde_value &value = plan_->values[i];
		const TypedBodyAbiType argument_abi =
			value.argument_index >= 0
				? typed_body_value_abi(plan_, i)
				: TypedBodyAbiType{};
		const bool exact_scalar =
			argument_abi.valid
			&& zend_mir_scalar_type_is_exact(
				argument_abi.exact_type)
			&& argument_abi.exact_type
				!= ZEND_MIR_SCALAR_TYPE_NULL;
		const bool unboxed_pointer =
			argument_abi.valid
			&& zend_tpde_machine_kind_is_pointer(argument_abi.machine_kind);
		const bool boxed_register_value =
			argument_abi.valid
			&& argument_abi.machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		if (value.argument_index < 0
				|| (!exact_scalar && !unboxed_pointer
					&& !boxed_register_value)) {
			continue;
		}
		if (function_mode_ == FunctionMode::TypedBody) {
			continue;
		}
		/*
		 * The canonical-locations value model names the Zend-frame
		 * location of every argument value; it never falls back to the
		 * frame argument ordinal.
		 */
		const zend_mir_storage_id storage_id =
			value.canonical_storage_id;
		if (!zend_mir_id_is_valid(storage_id)) {
			valid_ = false;
			continue;
		}
		if (!boxed_register_value) {
			argument_guards_.push_back({
				static_cast<uint32_t>(value.argument_index),
				storage_id,
				argument_abi.exact_type,
				argument_abi.machine_kind});
		}
		const uint32_t payload_reference = machine_reference_index(
			ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT, storage_id);
		if (payload_reference == UINT32_MAX) {
			valid_ = false;
			continue;
		}
		const IRValueRef payload_address = add_derived_value(
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE, storage_id, false, 0,
			UINT8_MAX, ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN,
			payload_reference);
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(payload_address);
		add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
			.kind = InstKind::ZvalPayloadLoad,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = static_cast<uint32_t>(value.argument_index),
			.result = IRValueRef{MIR_VALUE_BASE + i},
			.operand_offset = operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = storage_id,
			.exact_type = argument_abi.exact_type});
	}
}

void ZendIRAdaptor::Builder::select_component_results() {
	transient_scalar_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (!instruction.transient_scalar_result) {
			continue;
		}
		transient_scalar_results[i] = add_derived_value(
			instruction.transient_result_representation,
			instruction.transient_result_exact_type,
			instruction.transient_result_storage_id,
			false, 0,
			instruction.transient_result_machine_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_IMMORTAL);
	}
	/*
	 * Component-call results may feed PHIs that precede the call in MIR
	 * order (loop-carried direct-call results), so their local-ABI
	 * identities are selected before any PHI. Otherwise the PHI
	 * captures the canonical value and the call's later override leaves
	 * the backedge without a TPDE definition.
	 */
	register_component_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		const bool register_component_call =
			record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& instruction.direct_call != nullptr
			&& (frozen_typed_component_call(i)
				|| frozen_effect_closed_inline(i));
		if (!register_component_call) {
			continue;
		}

		/*
		 * A full-DFA result may reuse the destination CV's canonical
		 * binding while the source operand still names the defining SSA
		 * version. That definition wins, so a loop PHI's result is not
		 * replaced globally by its backedge call result.
		 */
		IRValueRef canonical_result =
			plan_source_operand_value_ref(
				plan_, instruction.direct_call->result_operand);
		if (canonical_result == INVALID_VALUE_REF) {
			canonical_result = source_binding_value_ref(
				instruction.source_result_binding);
		}
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		const zend_mir_value_id result_ssa =
			instruction.direct_call->result_operand
				.ssa_variable_id;
		TypedBodyAbiType result_abi =
			instruction.component_target_index
					< component_plans_.size()
				? (component_plans_[
						instruction.component_target_index]
						->return_abi)
				: TypedBodyAbiType{};
		if (canonical_result != INVALID_VALUE_REF
				&& is_mir_value(canonical_index)
				&& !result_abi.valid) {
			result_abi = typed_body_value_abi(
				plan_, canonical_index - MIR_VALUE_BASE);
		}
		if (!result_abi.valid
				&& zend_mir_scalar_type_is_exact(
					instruction.direct_call->result_type)
				&& instruction.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_NULL) {
			result_abi = zend_tpde_scalar_local_abi(
				instruction.direct_call->result_type);
		}
		if (!result_abi.valid) {
			valid_ = false;
			continue;
		}
		/*
		 * An exact null return has a valid local ABI but no payload to
		 * carry in a register: the canonical null value stays.
		 */
		if (result_abi.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			continue;
		}

		const IRValueRef result = add_derived_value(
			result_abi.representation,
			result_abi.exact_type, ZEND_MIR_ID_INVALID,
			false, 0, result_abi.machine_kind,
			local_abi_ownership(
				result_abi.transfer,
				canonical_result != INVALID_VALUE_REF
					&& is_mir_value(canonical_index)
				? plan_->values[
					canonical_index - MIR_VALUE_BASE].ownership
				: ZEND_MIR_OWNERSHIP_STATE_OWNED),
			local_abi_refcount(
				result_abi.transfer,
				canonical_result != INVALID_VALUE_REF
					&& is_mir_value(canonical_index)
				? plan_->values[
					canonical_index - MIR_VALUE_BASE].refcount_state
				: ZEND_MIR_REFCOUNT_UNKNOWN));
		register_component_results[i] = result;
		set_instruction_result(i, result);
		if (canonical_result != INVALID_VALUE_REF
				&& canonical_index >= MIR_VALUE_BASE
				&& canonical_index - MIR_VALUE_BASE
					< active_value_overrides().size()) {
			active_value_overrides()[
				canonical_index - MIR_VALUE_BASE] = result;
		}
		/* Without either identity the result is discarded. */
		if (result_ssa < active_source_ssa_overrides().size()) {
			active_source_ssa_overrides()[result_ssa] = result;
		}
	}
}

void ZendIRAdaptor::Builder::preselect_pointer_phis() {
	/*
	 * A source assignment can rename a private component-call result
	 * into a loop-carried CV. Its PHI precedes both the call and the
	 * assignment in MIR order, so the rename is selected here, where
	 * the call's local-ABI identity is known; otherwise the PHI
	 * backedge binds the canonical slot value and every iteration calls
	 * with the loop's initial argument. The assignment itself stays a
	 * guarded value operation: the hot edge publishes the register
	 * value to the CV, and the source temporary is materialized before
	 * the cold helper.
	 */
	preselected_phi_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	std::vector<zend_mir_storage_id> early_pointer_phi_storages;
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
				|| !instruction.has_value_operation
				|| instruction.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV
				|| guarded_cold_blocks[i] == UINT32_MAX) {
			continue;
		}
		const IRValueRef assigned = source_binding_value_ref(
			instruction.source_op2_binding);
		if (assigned == INVALID_VALUE_REF
				|| !zend_tpde_machine_kind_is_pointer(machine_kind(assigned))
				|| !machine_value_is_register_authoritative(
					assigned)
				|| !machine_value_has_register_definition(
					assigned)) {
			continue;
		}
		const zend_mir_storage_id source_storage =
			canonical_storage(assigned);
		if (zend_mir_id_is_valid(source_storage)) {
			early_pointer_phi_storages.push_back(source_storage);
		}
		const zend_mir_storage_id destination_storage =
			instruction.value_operation.op1_storage_id;
		if (zend_mir_id_is_valid(destination_storage)) {
			early_pointer_phi_storages.push_back(
				destination_storage);
		}
	}
	std::ranges::sort(early_pointer_phi_storages);
	early_pointer_phi_storages.erase(
		std::unique(early_pointer_phi_storages.begin(),
			early_pointer_phi_storages.end()),
		early_pointer_phi_storages.end());
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_mir_instruction_record phi =
			instruction_record_at(i);
		if (phi.opcode != ZEND_MIR_OPCODE_PHI
				|| phi.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan_, phi.result_id);
		const IRValueRef canonical =
			result_index < 0 ? INVALID_VALUE_REF
				: IRValueRef{MIR_VALUE_BASE
					+ static_cast<uint32_t>(result_index)};
		const bool pointer_phi =
			canonical != INVALID_VALUE_REF
			&& zend_tpde_machine_kind_is_pointer(machine_kind(canonical))
			&& machine_value_is_register_authoritative(canonical);
		if (canonical == INVALID_VALUE_REF || !pointer_phi) {
			continue;
		}
		const zend_mir_storage_id storage_id =
			canonical_storage(canonical);
		if (pointer_phi && !std::binary_search(
				early_pointer_phi_storages.begin(),
				early_pointer_phi_storages.end(), storage_id)) {
			continue;
		}
		const IRValueRef selected = add_derived_value(
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			exact_type(canonical), storage_id, false, 0,
			machine_kind(canonical),
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN);
		if (!zend_mir_id_is_valid(storage_id)) {
			valid_ = false;
			continue;
		}
		active_value_overrides()[
			static_cast<uint32_t>(result_index)] = selected;
		preselected_phi_results[i] = selected;
		if (zend_mir_value_is_original_ssa(
					plan_->values[result_index].id)
				&& plan_->values[result_index].id
					< active_source_ssa_overrides().size()) {
			active_source_ssa_overrides()[
				plan_->values[result_index].id] = selected;
		}
	}
}

/*
 * The assigned value becomes the CV's register definition, which
 * a boundary later stores into the slot. A CV that may hold a
 * reference (bound by global, static or &, or any global-scope
 * CV) keeps the value inside that reference.
 */
void ZendIRAdaptor::Builder::select_register_assignment(uint32_t i) {
	const zend_tpde_instruction &instruction =
		plan_->instructions[i];
	if (instruction.record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
			|| !instruction.has_value_operation
			|| instruction.value_operation.op1.slot_kind
				!= ZEND_MIR_SOURCE_SLOT_CV
			|| !instruction.source_op1_reference_free
			|| guarded_cold_blocks[i] == UINT32_MAX
			|| register_assignment_results[i]
				!= INVALID_VALUE_REF) {
		return;
	}
	const IRValueRef assigned = source_binding_value_ref(
		instruction.source_op2_binding);
	const uint32_t definition_plus_one =
		instruction.value_operation
			.op1_definition_ssa_variable_id_plus_one;
	if (assigned == INVALID_VALUE_REF
			|| definition_plus_one == 0
			|| !machine_value_is_register_authoritative(
				assigned)
			|| !machine_value_has_register_definition(assigned)) {
		return;
	}
	const uint32_t definition = definition_plus_one - 1;
	auto &source_overrides = active_source_ssa_overrides();
	if (definition >= source_overrides.size()) {
		valid_ = false;
		return;
	}
	const int32_t value_index = zend_tpde_value_index(
		plan_,
		zend_mir_value_from_original_ssa(definition));
	if (value_index < 0
			|| static_cast<uint32_t>(value_index)
				>= active_value_overrides().size()) {
		valid_ = false;
		return;
	}
	IRValueRef assignment_result = assigned;
	if (zend_tpde_machine_kind_is_pointer(machine_kind(assigned))) {
		uint64_t literal_length = 0;
		bool literal_truthy = false;
		const bool string_literal = known_string_literal(
			assigned, &literal_length, &literal_truthy);
		const uint8_t literal_first_byte = string_literal
				&& literal_length == 1 && !literal_truthy
			? '0' : 0;
		assignment_result = add_derived_value(
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			exact_type(assigned),
			instruction.value_operation.op1_storage_id,
			false, 0, machine_kind(assigned),
			ownership(assigned), refcount_state(assigned),
			UINT32_MAX, string_literal, literal_first_byte,
			literal_length);
	}
	register_assignment_sources[i] = assigned;
	register_assignment_results[i] = assignment_result;
	source_overrides[definition] = assignment_result;
	active_value_overrides()[
		static_cast<uint32_t>(value_index)] =
			assignment_result;
}

void ZendIRAdaptor::Builder::select_register_assignments() {
	register_assignment_sources.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	register_assignment_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		select_register_assignment(i);
	}
	std::vector<RegisterAssignmentPhiSource>
		register_assignment_phi_candidates;
	register_assignment_phi_candidates.reserve(
		register_assignment_results.size());
	for (uint32_t i = 0;
			i < register_assignment_results.size(); ++i) {
		const IRValueRef candidate =
			register_assignment_results[i];
		if (candidate == INVALID_VALUE_REF
				|| !zend_tpde_machine_kind_is_pointer(machine_kind(candidate))
				|| !plan_->instructions[i].has_value_operation) {
			continue;
		}
		register_assignment_phi_candidates.push_back({
			plan_->instructions[i].value_operation.op1_storage_id,
			candidate});
	}
	std::stable_sort(register_assignment_phi_candidates.begin(),
		register_assignment_phi_candidates.end(),
		[](const RegisterAssignmentPhiSource &left,
				const RegisterAssignmentPhiSource &right) {
			return left.storage_id < right.storage_id;
		});
	register_assignment_phi_sources.reserve(
		register_assignment_phi_candidates.size());
	for (uint32_t begin = 0;
			begin < register_assignment_phi_candidates.size();) {
		uint32_t end = begin + 1;
		IRValueRef selected =
			register_assignment_phi_candidates[begin].value;
		bool conflicting_kind = false;
		while (end < register_assignment_phi_candidates.size()
				&& register_assignment_phi_candidates[end].storage_id
					== register_assignment_phi_candidates[begin]
						.storage_id) {
			const IRValueRef candidate =
				register_assignment_phi_candidates[end].value;
			if (machine_kind(selected) != machine_kind(candidate)) {
				conflicting_kind = true;
			}
			selected = candidate;
			++end;
		}
		register_assignment_phi_sources.push_back({
			register_assignment_phi_candidates[begin].storage_id,
			conflicting_kind ? INVALID_VALUE_REF : selected});
		begin = end;
	}
}

IRValueRef ZendIRAdaptor::Builder::register_assignment_phi_source(
		zend_mir_storage_id storage_id) {
	const auto source = std::lower_bound(
		register_assignment_phi_sources.begin(),
		register_assignment_phi_sources.end(), storage_id,
		[](const RegisterAssignmentPhiSource &candidate,
				zend_mir_storage_id id) {
			return candidate.storage_id < id;
		});
	return source != register_assignment_phi_sources.end()
			&& source->storage_id == storage_id
		? source->value : INVALID_VALUE_REF;
}

/* A source result used by a machine consumer: -1 no consumer
 * yet, -2 several. */
void ZendIRAdaptor::Builder::mark_producer(
		int32_t producer, uint32_t consumer_index) {
	if (producer < 0
			|| static_cast<uint32_t>(producer)
				>= source_result_used.size()) {
		return;
	}
	const uint32_t producer_index =
		static_cast<uint32_t>(producer);
	source_result_used[producer_index] = 1;
	int32_t &recorded_consumer =
		source_result_consumer[producer_index];
	if (recorded_consumer == -1) {
		recorded_consumer = static_cast<int32_t>(consumer_index);
	} else if (recorded_consumer
			!= static_cast<int32_t>(consumer_index)) {
		recorded_consumer = -2;
	}
}

void ZendIRAdaptor::Builder::mark_binding(
		const zend_tpde_source_value_binding &binding,
		uint32_t consumer_index) {
	int32_t producer = binding.definition_instruction_index;
	if (producer < 0 && binding.value_index >= 0
			&& static_cast<uint32_t>(binding.value_index)
				< source_producer_by_value.size()) {
		producer = source_producer_by_value[
			static_cast<uint32_t>(binding.value_index)];
	}
	mark_producer(producer, consumer_index);
}

void ZendIRAdaptor::Builder::mark_source_result_consumers() {
	source_result_used.resize(plan_->instruction_count);
	source_result_consumer.assign(plan_->instruction_count, -1);
	source_result_materialized_for_helper.resize(plan_->instruction_count);
	source_producer_by_value.assign(plan_->value_count, -1);
	for (uint32_t consumer_index = 0;
			consumer_index < plan_->instruction_count;
			++consumer_index) {
		const zend_tpde_instruction &consumer =
			plan_->instructions[consumer_index];
		if (!consumer.has_value_operation) {
			continue;
		}
		if (consumer.source_result_binding.value_index >= 0
				&& static_cast<uint32_t>(
					consumer.source_result_binding.value_index)
						< source_producer_by_value.size()) {
			source_producer_by_value[
				static_cast<uint32_t>(
					consumer.source_result_binding.value_index)] =
				static_cast<int32_t>(consumer_index);
		}
		/*
		 * FE_FETCH defines its destination through op2, not the result
		 * operand: recording that producer lets an optimized binary
		 * consumer keep the fetched zval in the machine def-use graph.
		 */
		if (consumer.record.opcode
				== ZEND_MIR_OPCODE_ITERATOR_BRANCH
				&& consumer.source_op2_definition_binding.value_index >= 0
				&& static_cast<uint32_t>(
					consumer.source_op2_definition_binding.value_index)
					< source_producer_by_value.size()) {
			source_producer_by_value[
				static_cast<uint32_t>(
					consumer.source_op2_definition_binding.value_index)] =
				static_cast<int32_t>(consumer_index);
		}
		auto mark_source_producer =
			[&](const zend_tpde_source_value_binding &binding) {
				mark_binding(binding, consumer_index);
			};
		switch (consumer.record.opcode) {
			case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
			case ZEND_MIR_OPCODE_VALUE_ASSIGN:
				if (consumer.source_op2_binding.value_index >= 0
						|| consumer.source_op2_binding
							.definition_instruction_index >= 0) {
					mark_source_producer(
						consumer.source_op2_binding);
				} else {
					mark_source_producer(
						consumer.source_op1_binding);
				}
				break;
			case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
				mark_source_producer(
					consumer.source_op1_binding);
				break;
			case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
				/*
				 * Full OPcache DFA rewrites "$cv += fetch()" into a
				 * binary operation whose result defines the CV, so both
				 * operands are real consumers; the source result stays
				 * in registers for the binary selector's fast path.
				 */
				mark_source_producer(
					consumer.source_op1_binding);
				mark_source_producer(
					consumer.source_op2_binding);
				break;
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
			case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
				/*
				 * Guarded reads and isset/empty consume the array
				 * payload and the key directly.
				 */
				mark_source_producer(
					consumer.source_op1_binding);
				mark_source_producer(
					consumer.source_op2_binding);
				break;
			case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
			case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
			case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
				mark_source_producer(
					consumer.source_op1_binding);
				break;
			default:
				break;
		}
	}
	/*
	 * Calls consume their SEND operands through the frozen call-argument
	 * bindings, and every instruction its statepoint materializations;
	 * both are machine consumers of the producers they name. Recording
	 * them before register results are selected keeps a value OPcache
	 * compacted into a reused slot register-authoritative.
	 */
	for (uint32_t consumer_index = 0;
			consumer_index < plan_->instruction_count; ++consumer_index) {
		const zend_tpde_instruction &consumer =
			plan_->instructions[consumer_index];
		if ((consumer.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
				|| consumer.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER)
				&& plan_->call_argument_bindings != nullptr) {
			ZEND_ASSERT(consumer.call_argument_offset
					<= plan_->call_argument_count
				&& consumer.call_argument_count
					<= plan_->call_argument_count
						- consumer.call_argument_offset);
			for (uint32_t argument = 0;
					argument < consumer.call_argument_count; ++argument) {
				mark_binding(plan_->call_argument_bindings[
					consumer.call_argument_offset + argument],
					consumer_index);
			}
		}
		ZEND_ASSERT(consumer.materialization_offset
				<= plan_->materialization_count
			&& consumer.materialization_count
				<= plan_->materialization_count
					- consumer.materialization_offset);
		for (uint32_t materialization_index = 0;
				materialization_index < consumer.materialization_count;
				++materialization_index) {
			const zend_tpde_materialization &materialization =
				plan_->materializations[consumer.materialization_offset
					+ materialization_index];
			if (materialization.value_index == UINT32_MAX) {
				mark_producer(
					materialization.source_definition_instruction_index,
					consumer_index);
			}
		}
	}
}

bool ZendIRAdaptor::Builder::source_result_has_direct_consumer(
		uint32_t producer_index) {
	if (producer_index >= source_result_consumer.size()) {
		return false;
	}
	const int32_t signed_consumer =
		source_result_consumer[producer_index];
	if (signed_consumer <= static_cast<int32_t>(producer_index)
			|| static_cast<uint32_t>(signed_consumer)
				>= plan_->instruction_count) {
		return false;
	}
	const uint32_t consumer_index =
		static_cast<uint32_t>(signed_consumer);
	const uint32_t consumer_block =
		instruction_blocks[consumer_index];
	const uint32_t producer_continuation =
		guarded_continuation_blocks[producer_index];
	/*
	 * A guarded producer joins its fast and cold results in a
	 * generated continuation, where the next source instruction
	 * lives.
	 */
	uint32_t active_block = producer_continuation != UINT32_MAX
		? producer_continuation
		: instruction_blocks[producer_index];
	for (uint32_t intermediate = producer_index + 1;
			intermediate < consumer_index; ++intermediate) {
		const zend_mir_opcode opcode =
			instruction_record_at(intermediate).opcode;
		if (opcode == ZEND_MIR_OPCODE_CONSTANT
				|| opcode == ZEND_MIR_OPCODE_STATEPOINT) {
			continue;
		}
		const bool composable_boxed_read =
			(opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				|| opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
				|| opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
			&& source_result_consumer[intermediate]
				== signed_consumer
			&& guarded_continuation_blocks[intermediate]
				!= UINT32_MAX
			&& instruction_blocks[intermediate] == active_block;
		if (!composable_boxed_read) {
			return false;
		}
		active_block =
			guarded_continuation_blocks[intermediate];
	}
	return consumer_block == active_block;
}

void ZendIRAdaptor::Builder::select_register_boolean_results() {
	/*
	 * A full-DFA ADD/SUB may define a ZVAL inferred long|double. The long
	 * fast path and the overflow cold path both produce the complete
	 * payload/type-info pair, so such results are selected before PHIs: the
	 * backedge is then a boxed machine value when the loop join forms, and
	 * the PHI can become a TPDE value.
	 */
	register_binary_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	register_branch_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	register_boolean_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	/*
	 * Short-circuit PHIs precede their source operations in instruction
	 * order, so exact boolean results are selected first, including the
	 * edge result of JMPZ_EX/JMPNZ_EX: that is the condition itself on the
	 * only edge where the opcode defines its result, and needs no machine
	 * instruction.
	 */
	if (function_mode_ == FunctionMode::ZendEntry) {
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (!instruction.has_value_operation) {
				continue;
			}
			const bool register_boolean =
				(instruction.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
				&& (record.opcode
						== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
					|| (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
						&& (instruction.value_operation.source_opcode
								== ZEND_BOOL
							|| instruction.value_operation.source_opcode
								== ZEND_BOOL_NOT)));
			if (register_boolean) {
				const int32_t result_index =
					instruction.source_result_binding.value_index;
				const zend_mir_value_id result_ssa =
					instruction.value_operation.result.ssa_variable_id;
				const zend_mir_storage_id result_storage =
					instruction.value_operation.result_storage_id;
				if (!valid_value_index(result_index)
						|| result_ssa
							>= active_source_ssa_overrides().size()
						|| !zend_mir_id_is_valid(result_storage)) {
					valid_ = false;
					continue;
				}
				const bool result_alias =
					(instruction.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS) != 0;
				const IRValueRef selected = result_alias
					? source_binding_value_ref(
						instruction.source_op1_binding)
					: add_derived_value(
						ZEND_MIR_REPRESENTATION_I1,
						ZEND_MIR_SCALAR_TYPE_I1, result_storage,
						false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_IMMORTAL);
				if (selected == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				if (result_alias
						&& (exact_type(selected) != ZEND_MIR_SCALAR_TYPE_I1
							|| machine_kind(selected)
								!= ZEND_TPDE_MACHINE_VALUE_BOOL)) {
					valid_ = false;
					continue;
				}
				register_boolean_results[i] = selected;
				set_instruction_result(i, selected);
				active_value_overrides()[
					static_cast<uint32_t>(result_index)] = selected;
				active_source_ssa_overrides()[result_ssa] = selected;
				continue;
			}
			const uint32_t source_opcode =
				instruction.value_operation.source_opcode;
			if (record.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					|| (source_opcode != ZEND_JMPZ_EX
						&& source_opcode != ZEND_JMPNZ_EX)) {
				continue;
			}
			const IRValueRef condition = source_binding_value_ref(
				instruction.source_op1_binding);
			const int32_t result_index =
				instruction.source_result_binding.value_index;
			const zend_mir_value_id result_ssa =
				instruction.value_operation.result.ssa_variable_id;
			if (condition == INVALID_VALUE_REF
					|| exact_type(condition) != ZEND_MIR_SCALAR_TYPE_I1
					|| machine_kind(condition)
						!= ZEND_TPDE_MACHINE_VALUE_BOOL
					|| !machine_value_has_register_definition(condition)) {
				continue;
			}
			if (!valid_value_index(result_index)
					|| result_ssa >= active_source_ssa_overrides().size()) {
				valid_ = false;
				continue;
			}
			set_instruction_result(i, condition);
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = condition;
			active_source_ssa_overrides()[result_ssa] = condition;
		}
		/* The values a register JMPZ_EX/JMPNZ_EX defines on its edge. */
		std::vector<uint8_t> short_circuit_edge_values(
			plan_->value_count, 0);
		for (uint32_t producer_index = 0;
				producer_index < plan_->instruction_count;
				++producer_index) {
			const zend_tpde_instruction &producer =
				plan_->instructions[producer_index];
			const uint32_t source_opcode =
				producer.has_value_operation
					? producer.value_operation.source_opcode : 0;
			const int32_t edge_value =
				producer.source_result_binding.value_index;
			if (valid_value_index(edge_value)
					&& producer.record.opcode
						== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					&& (source_opcode == ZEND_JMPZ_EX
						|| source_opcode == ZEND_JMPNZ_EX)
					&& (producer.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH)
						!= 0) {
				short_circuit_edge_values[
					static_cast<uint32_t>(edge_value)] = 1;
			}
		}
		/*
		 * Only the two-edge short-circuit join: one input is the register
		 * edge result of JMPZ_EX/JMPNZ_EX, every input is an exact machine
		 * boolean, and the joined value only feeds a following condition.
		 * Loop-carried and general boolean PHIs are excluded.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_PHI
					|| instruction.operand_count != 2
					|| instruction.operand_offset
						> plan_->instruction_operand_count
					|| instruction.operand_count
						> plan_->instruction_operand_count
							- instruction.operand_offset) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan_, record.result_id);
			if (!valid_value_index(result_index)
					|| plan_->value_consumer_offsets == nullptr
					|| plan_->value_consumers == nullptr) {
				continue;
			}
			bool conditional_consumer = false;
			bool consumers_supported = true;
			const uint32_t consumer_begin =
				plan_->value_consumer_offsets[result_index];
			const uint32_t consumer_end =
				plan_->value_consumer_offsets[result_index + 1];
			for (uint32_t use_index = consumer_begin;
					use_index < consumer_end; ++use_index) {
				const zend_tpde_machine_use &use =
					plan_->value_consumers[use_index];
				if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
						|| use.operand_index != 0
						|| use.instruction_index
							>= plan_->instruction_count) {
					consumers_supported = false;
					break;
				}
				const zend_tpde_instruction &consumer =
					plan_->instructions[use.instruction_index];
				const zend_mir_instruction_record consumer_record =
					instruction_record_at(use.instruction_index);
				if (!consumer.has_value_operation
						|| consumer_record.opcode
							!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						|| consumer.source_op1_binding.value_index
							!= result_index) {
					consumers_supported = false;
					break;
				}
				conditional_consumer = true;
			}
			bool exact_register_inputs = true;
			bool short_circuit_edge = false;
			for (uint32_t n = 0; n < instruction.operand_count; ++n) {
				const zend_mir_value_id input_id =
					zend_tpde_operand_at(plan_, &instruction, n);
				const IRValueRef input = value_ref(input_id);
				if (input == INVALID_VALUE_REF
						|| exact_type(input) != ZEND_MIR_SCALAR_TYPE_I1
						|| machine_kind(input)
							!= ZEND_TPDE_MACHINE_VALUE_BOOL
						|| !machine_value_has_register_definition(input)) {
					exact_register_inputs = false;
					break;
				}
				const int32_t input_index =
					zend_tpde_value_index(plan_, input_id);
				if (valid_value_index(input_index)
						&& short_circuit_edge_values[
							static_cast<uint32_t>(input_index)] != 0) {
					short_circuit_edge = true;
				}
			}
			if (!conditional_consumer || !consumers_supported
					|| !exact_register_inputs || !short_circuit_edge) {
				continue;
			}
			const IRValueRef canonical_result{
				MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
			const zend_mir_storage_id result_storage =
				canonical_storage(canonical_result);
			if (!zend_mir_id_is_valid(result_storage)) {
				continue;
			}
			const IRValueRef selected = add_derived_value(
				ZEND_MIR_REPRESENTATION_I1,
				ZEND_MIR_SCALAR_TYPE_I1, result_storage,
				false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_IMMORTAL);
			register_boolean_results[i] = selected;
			preselected_phi_results[i] = selected;
			set_instruction_result(i, selected);
			active_value_overrides()[result_index] = selected;
			if (zend_mir_value_is_original_ssa(
					plan_->values[result_index].id)
					&& plan_->values[result_index].id
						< active_source_ssa_overrides().size()) {
				active_source_ssa_overrides()[
					plan_->values[result_index].id] = selected;
			}
		}
	}
}

void ZendIRAdaptor::Builder::collect_call_transport_storages() {
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (!machine_block_reachable(instruction_blocks[i])) {
			continue;
		}
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		const bool typed_call =
			record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& instruction.direct_call != nullptr
			&& frozen_typed_component_call(i);
		const bool scalar_inline =
			record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& instruction.direct_call != nullptr
			&& frozen_effect_closed_inline(i);
		const zend_tpde_plan *callee =
			instruction.component_target_index
					< component_plans_.size()
				? component_plans_[
					instruction.component_target_index]
				: nullptr;
		if ((!typed_call && !scalar_inline)
				|| callee == nullptr
				|| callee->argument_value_indices == nullptr
				|| plan_->call_argument_bindings == nullptr) {
			continue;
		}
		for (uint32_t argument_index = 0;
				argument_index < instruction.call_argument_count;
				++argument_index) {
			zend_mir_call_argument_ref argument{};
			const uint32_t call_argument_index =
				instruction.call_argument_offset + argument_index;
			const int32_t callee_value_index =
				argument_index < callee->argument_count
					? callee->argument_value_indices[argument_index]
					: -1;
			if (callee_value_index < 0
					|| static_cast<uint32_t>(callee_value_index)
						>= callee->value_count
					|| !zend_tpde_call_argument_at(plan_,
						call_argument_index, &argument)) {
				continue;
			}
			IRValueRef value = source_binding_value_ref(
				plan_->call_argument_bindings[
					call_argument_index]);
			if (value == INVALID_VALUE_REF) {
				value = source_operand_value_ref(
					argument.source_operand);
			}
			if (value == INVALID_VALUE_REF
					&& zend_mir_id_is_valid(argument.value_id)) {
				value = value_ref(argument.value_id);
			}
			if (value == INVALID_VALUE_REF
					|| machine_value_is_register_authoritative(value)
					|| !zend_mir_id_is_valid(
						canonical_storage(value))) {
				continue;
			}
			const TypedBodyAbiType transport_abi =
				typed_body_value_abi(
					callee,
					static_cast<uint32_t>(callee_value_index));
			const bool boxed_transport =
				typed_call && transport_abi.valid
				&& transport_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
			const bool scalar_transport =
				scalar_inline && transport_abi.valid
				&& representation(value)
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& (exact_type(value)
						== transport_abi.exact_type
					|| (exact_type(value)
							== ZEND_MIR_SCALAR_TYPE_NONE
						&& argument_index
							< instruction.direct_call
								->argument_count
						&& instruction.direct_call
								->arguments[argument_index].exact_type
							== transport_abi.exact_type))
				&& zend_tpde_machine_kind_is_long_or_bool(
					transport_abi.machine_kind);
			if (boxed_transport || scalar_transport) {
				register_boxed_storages.push_back(
					canonical_storage(value));
			}
		}
	}
}

void ZendIRAdaptor::Builder::select_register_binary_results() {
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (function_mode_ != FunctionMode::ZendEntry
				|| record.opcode
					!= ZEND_MIR_OPCODE_VALUE_BINARY_OP
				|| !instruction.has_value_operation
				|| guarded_cold_blocks[i] == UINT32_MAX) {
			continue;
		}
		const uint32_t source_opcode =
			instruction.value_operation.source_opcode;
		const bool boolean_result =
			long_comparison_source_opcode(source_opcode);
		if (!long_binary_source_opcode(source_opcode)) {
			continue;
		}
		const int32_t result_index =
			instruction.source_result_binding.value_index;
		const zend_mir_storage_id result_storage =
			instruction.value_operation.result_storage_id;
		const zend_mir_value_id result_ssa =
			instruction.value_operation.result.ssa_variable_id;
		if (!valid_value_index(result_index)
				|| !zend_mir_id_is_valid(result_storage)
				|| result_ssa
					>= active_source_ssa_overrides().size()) {
			continue;
		}
		const IRValueRef canonical{
			MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
		const bool scalar_result =
			(representation(canonical)
					== ZEND_MIR_REPRESENTATION_I64
				&& exact_type(canonical)
					== ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(canonical)
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| (representation(canonical)
					== ZEND_MIR_REPRESENTATION_I1
				&& exact_type(canonical)
					== ZEND_MIR_SCALAR_TYPE_I1
				&& machine_kind(canonical)
					== ZEND_TPDE_MACHINE_VALUE_BOOL);
		const bool boxed_result =
			representation(canonical)
				== ZEND_MIR_REPRESENTATION_ZVAL;
		if ((!scalar_result && !boxed_result)
				|| (scalar_result
					? machine_value_has_register_definition(canonical)
					: machine_value_has_result_representation(canonical))) {
			continue;
		}
		const IRValueRef selected = add_derived_value(
			scalar_result
				? representation(canonical)
				: ZEND_MIR_REPRESENTATION_ZVAL,
			scalar_result
				? exact_type(canonical)
				: ZEND_MIR_SCALAR_TYPE_NONE,
			result_storage, false, 0,
			scalar_result
				? machine_kind(canonical)
				: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			ownership(canonical), refcount_state(canonical));
		register_binary_results[i] = selected;
		set_instruction_result(i, selected);
		active_value_overrides()[
			static_cast<uint32_t>(result_index)] = selected;
		active_source_ssa_overrides()[result_ssa] = selected;
		if (boxed_result) {
			source_result_used[i] = 1;
			register_boxed_storages.push_back(result_storage);
		}
	}
}

void ZendIRAdaptor::Builder::select_register_branch_results() {
	/*
	 * Result-producing source branches define their result only on the
	 * taken edge. When that result is an exact scalar or pointer feeding a
	 * PHI, the branch instruction becomes its TPDE definition and the
	 * target captures the helper-produced payload before branching, instead
	 * of a predecessor loading it before the operation wrote the slot.
	 * Exact boolean short-circuit results were selected above; this handles
	 * the remaining JMP_SET/COALESCE shapes.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (!machine_block_reachable(instruction_blocks[i])) {
			continue;
		}
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		const int32_t result_index =
			instruction.source_result_binding.value_index;
		if (record.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| !instruction.has_value_operation
				|| (instruction.value_operation.source_opcode
						!= ZEND_JMP_SET
					&& instruction.value_operation.source_opcode
						!= ZEND_COALESCE)
				|| !valid_value_index(result_index)
				|| !zend_mir_id_is_valid(
					instruction.value_operation.result_storage_id)) {
			continue;
		}
		const IRValueRef canonical{
			MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
		const bool scalar_result =
			(representation(canonical)
					== ZEND_MIR_REPRESENTATION_I64
				&& exact_type(canonical)
					== ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(canonical)
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| (representation(canonical)
					== ZEND_MIR_REPRESENTATION_I1
				&& exact_type(canonical)
					== ZEND_MIR_SCALAR_TYPE_I1
				&& machine_kind(canonical)
					== ZEND_TPDE_MACHINE_VALUE_BOOL);
		const bool pointer_result =
			zend_tpde_machine_kind_is_pointer(machine_kind(canonical));
		if ((!scalar_result && !pointer_result)
				|| machine_value_has_register_definition(canonical)) {
			continue;
		}
		const zend_mir_value_id result_ssa =
			instruction.value_operation.result.ssa_variable_id;
		if (result_ssa >= active_source_ssa_overrides().size()) {
			valid_ = false;
			continue;
		}
		const IRValueRef selected = add_derived_value(
			pointer_result
				? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				: representation(canonical),
			exact_type(canonical),
			instruction.value_operation.result_storage_id,
			false, 0, machine_kind(canonical),
			ownership(canonical), refcount_state(canonical));
		register_branch_results[i] = selected;
		set_instruction_result(i, selected);
		active_value_overrides()[
			static_cast<uint32_t>(result_index)] = selected;
		active_source_ssa_overrides()[result_ssa] = selected;
	}
}

void ZendIRAdaptor::Builder::select_packed_fetch_results() {
	/*
	 * FE_FETCH writes its destination on the taken edge, and full OPcache
	 * DFA can feed that definition straight into a binary operation,
	 * bypassing the assignment that would keep the value
	 * register-authoritative. The packed-fetch shape therefore publishes a
	 * boxed branch result; both targets reload the canonical zval after the
	 * fast path or the helper.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		zend_tpde_packed_iterator_fetch iterator{};
		const int32_t result_index =
			instruction.source_op2_definition_binding.value_index;
		if (function_mode_ != FunctionMode::ZendEntry
				|| instruction.record.opcode
					!= ZEND_MIR_OPCODE_ITERATOR_BRANCH
				|| source_result_used[i] == 0
				|| !valid_value_index(result_index)
				|| !zend_tpde_packed_iterator_fetch_at(
					instruction, &iterator)
				|| !zend_mir_id_is_valid(
					instruction.value_operation.op2_storage_id)
				|| instruction
					.source_op2_definition_ssa_variable_id_plus_one == 0) {
			continue;
		}
		const IRValueRef canonical{
			MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
		if (representation(canonical) != ZEND_MIR_REPRESENTATION_ZVAL
				|| machine_value_has_register_definition(canonical)) {
			continue;
		}
		const zend_mir_value_id result_ssa =
			instruction
				.source_op2_definition_ssa_variable_id_plus_one - 1;
		if (result_ssa >= active_source_ssa_overrides().size()) {
			valid_ = false;
			continue;
		}
		const IRValueRef selected = add_derived_value(
			iterator.destination_scalar_only
				? ZEND_MIR_REPRESENTATION_I64
				: ZEND_MIR_REPRESENTATION_ZVAL,
			iterator.destination_scalar_only
				? ZEND_MIR_SCALAR_TYPE_I64
				: ZEND_MIR_SCALAR_TYPE_NONE,
			instruction.value_operation.op2_storage_id,
			false, 0, iterator.destination_scalar_only
				? ZEND_TPDE_MACHINE_VALUE_I64
				: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			iterator.destination_scalar_only
				? ZEND_MIR_OWNERSHIP_STATE_BORROWED
				: ownership(canonical),
			iterator.destination_scalar_only
				? ZEND_MIR_REFCOUNT_IMMORTAL
				: refcount_state(canonical));
		register_branch_results[i] = selected;
		set_instruction_result(i, selected);
		active_value_overrides()[
			static_cast<uint32_t>(result_index)] = selected;
		active_source_ssa_overrides()[result_ssa] = selected;
	}
}

void ZendIRAdaptor::Builder::close_register_boxed_storages() {
	/*
	 * Close the register-boxed storages over assignments: the source of
	 * an assignment to one is one too. Only membership matters; the
	 * storages are sorted where they are consumed.
	 */
	{
		std::unordered_map<zend_mir_storage_id,
			std::vector<zend_mir_storage_id>> assignment_sources;
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (instruction_record_at(i).opcode
						!= ZEND_MIR_OPCODE_VALUE_ASSIGN
					|| !instruction.has_value_operation) {
				continue;
			}
			IRValueRef source = source_binding_value_ref(
				instruction.source_op2_binding);
			if (source == INVALID_VALUE_REF) {
				source = source_operand_value_ref(
					instruction.value_operation.op2);
			}
			const zend_mir_storage_id source_storage =
				source == INVALID_VALUE_REF
					? ZEND_MIR_ID_INVALID
					: canonical_storage(source);
			if (zend_mir_id_is_valid(source_storage)) {
				assignment_sources[
					instruction.value_operation.op1_storage_id]
					.push_back(source_storage);
			}
		}
		std::unordered_set<zend_mir_storage_id> boxed(
			register_boxed_storages.begin(),
			register_boxed_storages.end());
		std::vector<zend_mir_storage_id> pending(
			boxed.begin(), boxed.end());
		while (!pending.empty()) {
			const zend_mir_storage_id storage = pending.back();
			pending.pop_back();
			const auto sources = assignment_sources.find(storage);
			if (sources == assignment_sources.end()) {
				continue;
			}
			for (const zend_mir_storage_id source : sources->second) {
				if (boxed.insert(source).second) {
					register_boxed_storages.push_back(source);
					pending.push_back(source);
				}
			}
		}
	}
}

void ZendIRAdaptor::Builder::reselect_register_assignments() {
	/*
	 * Binary and branch results are selected after the first assignment
	 * pass: assignments whose source became register-authoritative there
	 * are revisited, so later PHI-edge copies keep that definition.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		select_register_assignment(i);
	}
}

void ZendIRAdaptor::Builder::select_mutation_result(uint32_t i) {
	if (mutation_results[i] != INVALID_VALUE_REF) {
		return;
	}
	const zend_tpde_instruction &instruction =
		plan_->instructions[i];
	if (guarded_cold_blocks[i] == UINT32_MAX
			&& std::binary_search(
				frame_authoritative_phi_storages.begin(),
				frame_authoritative_phi_storages.end(),
				instruction.value_operation.op1_storage_id)) {
		return;
	}
	const IRValueRef canonical =
		mutation_value_ref(instruction);
	if (canonical == INVALID_VALUE_REF) {
		return;
	}
	if (representation(canonical)
				!= ZEND_MIR_REPRESENTATION_ZVAL
			|| !zend_mir_id_is_valid(
				canonical_storage(canonical))
			|| canonical_storage(canonical)
				!= instruction.value_operation.op1_storage_id
			|| instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one == 0) {
		valid_ = false;
		return;
	}
	const zend_mir_value_id mutation_ssa =
		instruction.value_operation
			.op1_definition_ssa_variable_id_plus_one - 1;
	const int32_t mutation_index = zend_tpde_value_index(
		plan_, zend_mir_value_from_original_ssa(mutation_ssa));
	const IRValueRef mutation = add_derived_value(
		ZEND_MIR_REPRESENTATION_ZVAL,
		ZEND_MIR_SCALAR_TYPE_NONE,
		instruction.value_operation.op1_storage_id,
		false, 0, ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
		ownership(canonical), refcount_state(canonical));
	if (mutation_index < 0
			|| static_cast<uint32_t>(mutation_index)
				>= active_value_overrides().size()
			|| mutation_ssa
				>= active_source_ssa_overrides().size()) {
		valid_ = false;
		return;
	}
	active_value_overrides()[
		static_cast<uint32_t>(mutation_index)] = mutation;
	active_source_ssa_overrides()[mutation_ssa] = mutation;
	set_instruction_result(i, mutation);
	mutation_results[i] = mutation;
	lazy_mutation_storages.push_back(
		instruction.value_operation.op1_storage_id);
}

void ZendIRAdaptor::Builder::select_mutation_results() {
	/*
	 * Guarded in-place mutations come after their loop PHIs in MIR order.
	 * Their boxed machine results are selected first, so a loop-carried
	 * zval becomes a TPDE PHI instead of a frame reload cycle. An earlier
	 * result-less mutation of the same non-reference-bound storage must use
	 * that identity too, or the PHI sees the guarded result on only one
	 * edge.
	 */
	mutation_results.assign(
		plan_->instruction_count, INVALID_VALUE_REF);
	std::vector<zend_mir_storage_id> related_mutation_storages;
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (instruction.mutation_lazy_scalar
				&& guarded_cold_blocks[i] != UINT32_MAX
				&& mutation_value_ref(instruction)
					!= INVALID_VALUE_REF) {
			related_mutation_storages.push_back(
				instruction.value_operation.op1_storage_id);
		}
	}
	std::ranges::sort(related_mutation_storages);
	related_mutation_storages.erase(
		std::unique(related_mutation_storages.begin(),
			related_mutation_storages.end()),
		related_mutation_storages.end());
	std::erase_if(related_mutation_storages,
		[&](zend_mir_storage_id storage_id) {
			return storage_assigned_by_reference(storage_id);
		});
	/*
	 * A result-bearing post-inc/dec exposes the value before the mutation
	 * as its result. When that result is part of a conditional assignment
	 * chain, the assignment source has no machine definition for the
	 * updated loop-carried value, so every PHI and mutation of the chain
	 * stays frame-authoritative: selecting part of it would let later
	 * operations consume a register while an outgoing edge reloads the
	 * slot. Standalone post-inc/dec loops stay register-authoritative.
	 */
	std::vector<zend_mir_storage_id> consumed_post_incdec_phi_storages;
	std::vector<zend_mir_storage_id> zval_phi_storages;
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (record.opcode != ZEND_MIR_OPCODE_PHI
				|| record.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const int32_t source_block = block_index(record.block_id);
		if (source_block < 0) {
			continue;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[source_block];
		const uint32_t predecessor_count =
			plan_->block_predecessor_offsets[source_block + 1]
				- predecessor_begin;
		bool loop_phi = false;
		for (uint32_t n = 0; n < predecessor_count; ++n) {
			const uint32_t predecessor =
				plan_->block_predecessors[predecessor_begin + n];
			if (predecessor < plan_->block_count
					&& machine_block_dominates(
						instruction_blocks[i],
						final_blocks[predecessor])) {
				loop_phi = true;
				break;
			}
		}
		if (!loop_phi) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan_, record.result_id);
		if (result_index < 0) {
			continue;
		}
		const zend_mir_storage_id storage_id = canonical_storage(
			IRValueRef{MIR_VALUE_BASE
				+ static_cast<uint32_t>(result_index)});
		if (zend_mir_id_is_valid(storage_id)) {
			zval_phi_storages.push_back(storage_id);
		}
	}
	std::ranges::sort(zval_phi_storages);
	zval_phi_storages.erase(
		std::unique(zval_phi_storages.begin(), zval_phi_storages.end()),
		zval_phi_storages.end());
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan_->instructions[i];
		const zend_mir_storage_id storage_id =
			instruction.has_value_operation
				? instruction.value_operation.op1_storage_id
				: ZEND_MIR_ID_INVALID;
		if (!std::binary_search(zval_phi_storages.begin(),
				zval_phi_storages.end(), storage_id)) {
			continue;
		}
		zend_tpde_long_incdec incdec{};
		if (instruction.has_value_operation
				&& zend_tpde_long_incdec_at(instruction, &incdec)
				&& incdec.has_result
				&& source_result_used[i] != 0
				&& zend_mir_id_is_valid(storage_id)) {
			consumed_post_incdec_phi_storages.push_back(storage_id);
		}
		if (instruction.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
				&& instruction.has_value_operation
				&& zend_mir_id_is_valid(storage_id)
				&& guarded_cold_blocks[i] != UINT32_MAX
				&& std::binary_search(related_mutation_storages.begin(),
					related_mutation_storages.end(), storage_id)
				&& (register_assignment_results[i] == INVALID_VALUE_REF
					|| !machine_value_has_register_definition(
						register_assignment_results[i]))) {
			frame_authoritative_phi_storages.push_back(storage_id);
		}
	}
	if (!frame_authoritative_phi_storages.empty()) {
		frame_authoritative_phi_storages.insert(
			frame_authoritative_phi_storages.end(),
			consumed_post_incdec_phi_storages.begin(),
			consumed_post_incdec_phi_storages.end());
	}
	std::ranges::sort(frame_authoritative_phi_storages);
	frame_authoritative_phi_storages.erase(
		std::unique(frame_authoritative_phi_storages.begin(),
			frame_authoritative_phi_storages.end()),
		frame_authoritative_phi_storages.end());
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const bool guarded_mutation =
			instruction.mutation_lazy_scalar
			&& guarded_cold_blocks[i] != UINT32_MAX;
		const bool related_mutation =
			instruction.has_value_operation
				&& std::ranges::binary_search(
					related_mutation_storages,
					instruction.value_operation.op1_storage_id);
		if (guarded_mutation || related_mutation) {
			select_mutation_result(i);
		}
	}
	lazy_mutation_storages.insert(
		lazy_mutation_storages.end(),
		register_boxed_storages.begin(),
		register_boxed_storages.end());
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (instruction.zval_store_lazy_scalar
				&& zend_mir_id_is_valid(
					instruction.zval_store_storage_id)) {
			lazy_mutation_storages.push_back(
				instruction.zval_store_storage_id);
		}
	}
	std::ranges::sort(lazy_mutation_storages);
	lazy_mutation_storages.erase(
		std::unique(lazy_mutation_storages.begin(),
			lazy_mutation_storages.end()),
		lazy_mutation_storages.end());
}

bool ZendIRAdaptor::Builder::boxed_phi_needs_scalar_transport(
		const zend_tpde_instruction &instruction,
		zend_mir_storage_id result_storage) {
	if (!zend_mir_id_is_valid(result_storage)) {
		return false;
	}
	for (uint32_t operand = 0;
			operand < instruction.operand_count; ++operand) {
		const zend_mir_value_id input_id =
			zend_tpde_operand_at(plan_, &instruction, operand);
		const IRValueRef input = value_ref(input_id);
		if (input == INVALID_VALUE_REF
				|| canonical_storage(input) == result_storage) {
			continue;
		}
		const zend_mir_scalar_type_mask input_type = exact_type(input);
		const bool boxable_scalar =
			(input_type == ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(input)
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
				&& machine_kind(input)
					== ZEND_TPDE_MACHINE_VALUE_BOOL)
			|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
				&& machine_kind(input)
					== ZEND_TPDE_MACHINE_VALUE_F64);
		uint64_t constant_bits;
		if (boxable_scalar
				&& ((machine_value_is_register_authoritative(input)
						&& machine_value_has_register_definition(input))
					|| constant(input, &constant_bits))) {
			return true;
		}
	}
	return false;
}

bool ZendIRAdaptor::Builder::phi_uses_runtime_short_circuit_slot(
		const zend_tpde_instruction &instruction,
		zend_mir_storage_id result_storage) {
	if (!zend_mir_id_is_valid(result_storage)) {
		return false;
	}
	bool has_runtime_short_circuit_input = false;
	for (uint32_t operand = 0;
			operand < instruction.operand_count; ++operand) {
		const zend_mir_value_id input_id =
			zend_tpde_operand_at(plan_, &instruction, operand);
		const int32_t input_index =
			zend_tpde_value_index(plan_, input_id);
		const IRValueRef input = value_ref(input_id);
		if (input_index < 0 || input == INVALID_VALUE_REF
				|| canonical_storage(input) != result_storage) {
			return false;
		}
		const int32_t definition =
			plan_->value_definition_instructions != nullptr
			? plan_->value_definition_instructions[input_index] : -1;
		bool runtime_short_circuit_input = false;
		if (definition >= 0
				&& static_cast<uint32_t>(definition)
					< plan_->instruction_count) {
			const zend_tpde_instruction &producer =
				plan_->instructions[static_cast<uint32_t>(definition)];
			runtime_short_circuit_input =
				producer.record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& producer.has_value_operation
				&& (producer.value_operation.source_opcode
						== ZEND_COALESCE
					|| producer.value_operation.source_opcode
						== ZEND_JMP_SET);
		}
		if (!runtime_short_circuit_input) {
			runtime_short_circuit_input =
				runtime_short_circuit_result_values[
					static_cast<uint32_t>(input_index)] != 0;
		}
		has_runtime_short_circuit_input |=
			runtime_short_circuit_input;
	}
	return has_runtime_short_circuit_input;
}

void ZendIRAdaptor::Builder::select_phi_results() {
	boxed_phi_input_overrides.assign(
		plan_->instruction_operand_count, INVALID_VALUE_REF);
	boxed_phi_cold_input_overrides.assign(
		plan_->instruction_operand_count, INVALID_VALUE_REF);
	selected_phi_results = preselected_phi_results;
	runtime_short_circuit_result_values.assign(
		plan_->value_count, 0);
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &producer = plan_->instructions[i];
		const int32_t result_index =
			producer.source_result_binding.value_index;
		if (producer.record.opcode
				== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& producer.has_value_operation
				&& valid_value_index(result_index)
				&& (producer.value_operation.source_opcode
						== ZEND_COALESCE
					|| producer.value_operation.source_opcode
						== ZEND_JMP_SET)) {
			runtime_short_circuit_result_values[
				static_cast<uint32_t>(result_index)] = 1;
		}
	}
	/*
	 * Register assignments and lazy scalar mutations may close an outer
	 * loop through nested ZVAL PHIs, which are not emitted in def-use
	 * order. Every PHI of such a CV gets its final machine identity before
	 * any incoming edge is resolved; otherwise an inner PHI met before its
	 * outer input PHI reloads the stale slot.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (record.opcode != ZEND_MIR_OPCODE_PHI
				|| record.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan_, record.result_id);
		const IRValueRef canonical_result =
			result_index < 0 ? INVALID_VALUE_REF
				: IRValueRef{
					MIR_VALUE_BASE
						+ static_cast<uint32_t>(result_index)};
		const zend_mir_storage_id storage_id =
			canonical_result == INVALID_VALUE_REF
				? ZEND_MIR_ID_INVALID
				: canonical_storage(canonical_result);
		const IRValueRef assignment_source =
			register_assignment_phi_source(storage_id);
		const bool scalar_transport_phi =
			boxed_phi_needs_scalar_transport(
				plan_->instructions[i], storage_id);
		const bool runtime_short_circuit_slot_phi =
			phi_uses_runtime_short_circuit_slot(
				plan_->instructions[i], storage_id);
		const bool boxed_phi =
			zend_mir_id_is_valid(storage_id)
			&& !runtime_short_circuit_slot_phi
			&& !std::binary_search(
				frame_authoritative_phi_storages.begin(),
				frame_authoritative_phi_storages.end(), storage_id)
			&& (scalar_transport_phi
				|| std::binary_search(
					lazy_mutation_storages.begin(),
					lazy_mutation_storages.end(), storage_id));
		const bool pointer_phi =
			assignment_source != INVALID_VALUE_REF
			&& canonical_result != INVALID_VALUE_REF
			&& zend_tpde_machine_kind_is_pointer(
				machine_kind(canonical_result))
			&& machine_kind(canonical_result)
				== machine_kind(assignment_source)
			&& machine_value_is_register_authoritative(
				canonical_result);
		if (!zend_mir_id_is_valid(storage_id)
				|| canonical_result == INVALID_VALUE_REF
				|| exact_type(canonical_result)
					== ZEND_MIR_SCALAR_TYPE_NULL
				|| (!pointer_phi && !boxed_phi)) {
			continue;
		}
		const IRValueRef selected_phi = add_derived_value(
			pointer_phi
				? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				: ZEND_MIR_REPRESENTATION_ZVAL,
			pointer_phi
				? exact_type(assignment_source)
				: ZEND_MIR_SCALAR_TYPE_NONE,
			storage_id, false, 0,
			pointer_phi
				? machine_kind(assignment_source)
				: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			pointer_phi
				? ZEND_MIR_OWNERSHIP_STATE_BORROWED
				: ownership(canonical_result),
			pointer_phi
				? ZEND_MIR_REFCOUNT_UNKNOWN
				: refcount_state(canonical_result));
		if (result_index < 0
				|| static_cast<uint32_t>(result_index)
					>= active_value_overrides().size()) {
			valid_ = false;
			continue;
		}
		selected_phi_results[i] = selected_phi;
	}
	for (uint32_t i = 0; i < selected_phi_results.size(); ++i) {
		selected_phi_definitions.add(i, selected_phi_results[i]);
	}
	/* The register assignments are final here. */
	for (uint32_t i = 0; i < register_assignment_results.size(); ++i) {
		register_assignment_definitions.add(
			i, register_assignment_results[i]);
	}
}

IRValueRef ZendIRAdaptor::Builder::value_ref_for_block(
		zend_mir_value_id value_id, uint32_t use_block,
		uint32_t use_instruction) {
	const int32_t value_index = zend_tpde_value_index(plan_, value_id);
	if (value_index < 0) {
		return INVALID_VALUE_REF;
	}
	const IRValueRef canonical{
		MIR_VALUE_BASE + static_cast<uint32_t>(value_index)};
	const IRValueRef selected = value_ref(value_id);
	if (selected == INVALID_VALUE_REF || selected == canonical) {
		return selected;
	}
	bool reaches = false;
	instruction_result_definitions_.visit(active_instruction_results(),
		selected, [&](uint32_t definition) {
			reaches = reaches
				|| (machine_block_dominates(
						instruction_blocks[definition], use_block)
					&& (instruction_blocks[definition] != use_block
						|| use_instruction == UINT32_MAX
						|| definition <= use_instruction));
		});
	selected_phi_definitions.visit(selected_phi_results, selected,
		[&](uint32_t definition) {
			reaches = reaches || machine_block_dominates(
				instruction_blocks[definition], use_block);
		});
	return reaches ? selected : canonical;
}

/*
 * A source binding without its own definition resolves through the
 * global override chain, which a reused SSA identity may have pointed
 * at a derived value defined on another path. A register operand is
 * usable only where one of its definitions dominates the use.
 */
bool ZendIRAdaptor::Builder::register_operand_reaches(
		IRValueRef value, uint32_t use_block, uint32_t use_instruction) {
	if (derived_value(value) == nullptr) {
		return true;
	}
	bool defined = false;
	bool reaches = false;
	instruction_result_definitions_.visit(active_instruction_results(),
		value, [&](uint32_t definition) {
			defined = true;
			reaches = reaches
				|| (machine_block_dominates(
						instruction_blocks[definition], use_block)
					&& (instruction_blocks[definition] != use_block
						|| definition < use_instruction));
		});
	selected_phi_definitions.visit(selected_phi_results, value,
		[&](uint32_t definition) {
			defined = true;
			reaches = reaches || machine_block_dominates(
				instruction_blocks[definition], use_block);
		});
	return reaches || !defined;
}

bool ZendIRAdaptor::Builder::entry_argument_has_register_definition(
		IRValueRef value) {
	if (function_mode_ != FunctionMode::ZendEntry) {
		return false;
	}
	const uint32_t raw = static_cast<uint32_t>(value);
	if (!is_mir_value(raw)) {
		return false;
	}
	const uint32_t value_index = raw - MIR_VALUE_BASE;
	const zend_tpde_value &plan_value = plan_->values[value_index];
	if (plan_value.argument_index < 0
			|| plan_value.representation
				== ZEND_MIR_REPRESENTATION_ZVAL) {
		return false;
	}
	const TypedBodyAbiType argument_abi =
		typed_body_value_abi(plan_, value_index);
	return argument_abi.valid
		&& ((zend_mir_scalar_type_is_exact(argument_abi.exact_type)
				&& argument_abi.exact_type
					!= ZEND_MIR_SCALAR_TYPE_NULL)
			|| zend_tpde_machine_kind_is_pointer(argument_abi.machine_kind)
			|| argument_abi.machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
}

IRValueRef ZendIRAdaptor::Builder::resolve_materializable_scalar(
		uint32_t value_index, uint32_t use_block, uint32_t use_instruction,
		uint32_t depth) {
	if (value_index >= plan_->value_count
			|| depth > plan_->value_count) {
		return INVALID_VALUE_REF;
	}
	const zend_tpde_value &plan_value = plan_->values[value_index];
	IRValueRef value = value_ref_for_block(
		plan_value.id, use_block, use_instruction);
	const int32_t definition =
		plan_->value_definition_instructions == nullptr
			? -1
			: plan_->value_definition_instructions[value_index];
	if (definition >= 0
			&& static_cast<uint32_t>(definition)
				< plan_->instruction_count) {
		const zend_tpde_instruction &copy =
			plan_->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_instruction_record copy_record =
			instruction_record_at(static_cast<uint32_t>(definition));
		if (copy_record.opcode == ZEND_MIR_OPCODE_COPY
				&& copy.operand_count == 1
				&& !plan_value.canonical_alias_observable
				&& plan_value.representation
					== ZEND_MIR_REPRESENTATION_ZVAL) {
			const zend_mir_value_id input_id =
				zend_tpde_operand_at(plan_, &copy, 0);
			const int32_t input_index =
				zend_tpde_value_index(plan_, input_id);
			const IRValueRef copied_input = value_ref(input_id);
			if (input_index >= 0
					&& copied_input != INVALID_VALUE_REF
					&& plan_->values[input_index].representation
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& zend_mir_id_is_valid(
						plan_value.canonical_storage_id)
					&& plan_->values[input_index].canonical_storage_id
						== plan_value.canonical_storage_id
					&& register_assignment_definitions.contains(
						register_assignment_results, copied_input)) {
				return resolve_materializable_scalar(
					static_cast<uint32_t>(input_index),
					use_block, use_instruction, depth + 1);
			}
		}
	}
	if (machine_value_has_result_representation(value)
			&& (machine_value_has_register_definition(value)
				|| entry_argument_has_register_definition(value))) {
		bool assignment_result = false;
		bool assignment_dominates = false;
		register_assignment_definitions.visit(
			register_assignment_results, value,
			[&](uint32_t assignment_index) {
				assignment_result = true;
				const uint32_t assignment_block =
					guarded_continuation_blocks[assignment_index]
							!= UINT32_MAX
						? guarded_continuation_blocks[assignment_index]
						: instruction_blocks[assignment_index];
				assignment_dominates |= machine_block_dominates(
					assignment_block, use_block);
			});
		if (assignment_result && !assignment_dominates) {
			return INVALID_VALUE_REF;
		}
		return value;
	}
	if (definition < 0
			|| static_cast<uint32_t>(definition)
				>= plan_->instruction_count) {
		return INVALID_VALUE_REF;
	}
	const uint32_t definition_index =
		static_cast<uint32_t>(definition);
	/*
	 * Boxed PHIs are selected before the instruction stream is built so
	 * their incoming-edge conversions are known, and a statepoint may
	 * use such a PHI before active_instruction_results records its
	 * instruction. The selected value is defined at block entry and is
	 * the materialization source.
	 */
	if (definition_index < selected_phi_results.size()) {
		const IRValueRef selected =
			selected_phi_results[definition_index];
		if (selected != INVALID_VALUE_REF
				&& machine_value_has_result_representation(selected)
				&& machine_value_has_register_definition(selected)
				&& machine_block_dominates(
					instruction_blocks[definition_index], use_block)) {
			return selected;
		}
	}
	if (definition_index < active_instruction_results().size()) {
		const IRValueRef selected =
			active_instruction_results()[definition_index];
		if (selected != value
				&& machine_value_has_result_representation(selected)
				&& machine_value_has_register_definition(selected)
				&& machine_block_dominates(
					instruction_blocks[definition_index], use_block)
				&& (instruction_blocks[definition_index] != use_block
					|| use_instruction == UINT32_MAX
					|| definition_index <= use_instruction)) {
			return selected;
		}
	}
	const zend_tpde_instruction &instruction =
		plan_->instructions[definition_index];
	const zend_mir_instruction_record record =
		instruction_record_at(definition_index);
	if (record.opcode != ZEND_MIR_OPCODE_COPY
			|| instruction.operand_count != 1
			|| plan_value.canonical_alias_observable
			|| !zend_mir_scalar_type_is_exact(plan_value.exact_type)
			|| plan_value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
		return INVALID_VALUE_REF;
	}
	const zend_mir_value_id input_id =
		zend_tpde_operand_at(plan_, &instruction, 0);
	const int32_t input_index =
		zend_tpde_value_index(plan_, input_id);
	if (input_index < 0
			|| plan_->values[input_index].exact_type
			!= plan_value.exact_type) {
		return INVALID_VALUE_REF;
	}
	/*
	 * A COPY may move an exact scalar from a literal or register into
	 * another canonical slot, and scalar-definition freezing may
	 * promote a boxed COPY: both are traced, so forward statepoint
	 * materializations reach the PHI, argument or constant definition.
	 */
	const IRValueRef resolved = resolve_materializable_scalar(
		static_cast<uint32_t>(input_index), use_block,
		use_instruction, depth + 1);
	if (resolved == INVALID_VALUE_REF
			|| (!machine_value_has_result_representation(resolved)
				&& plan_->values[input_index].canonical_storage_id
					!= plan_value.canonical_storage_id)) {
		return INVALID_VALUE_REF;
	}
	return resolved;
}

bool ZendIRAdaptor::Builder::entry_register_definition(IRValueRef value) {
	const uint32_t raw = static_cast<uint32_t>(value);
	if (function_mode_ != FunctionMode::ZendEntry
			|| value == INVALID_VALUE_REF
			|| derived_value(value) != nullptr
			|| !is_mir_value(raw)
			|| plan_->value_definition_instructions == nullptr
			|| !machine_value_is_register_authoritative(value)) {
		return false;
	}
	const int32_t definition =
		plan_->value_definition_instructions[raw - MIR_VALUE_BASE];
	return definition >= 0
		&& static_cast<uint32_t>(definition)
			< entry_register_definitions.size()
		&& entry_register_definitions[
			static_cast<uint32_t>(definition)] != 0;
}

IRValueRef ZendIRAdaptor::Builder::resolve_copy_input(
		zend_mir_value_id value_id, uint32_t use_block,
		uint32_t use_instruction) {
	const int32_t value_index =
		zend_tpde_value_index(plan_, value_id);
	if (value_index < 0) {
		return value_ref(value_id);
	}
	const IRValueRef resolved = resolve_materializable_scalar(
		static_cast<uint32_t>(value_index), use_block,
		use_instruction, 0);
	if (resolved == INVALID_VALUE_REF
			&& entry_register_definition(value_ref(value_id))) {
		return value_ref(value_id);
	}
	return resolved;
}

IRValueRef ZendIRAdaptor::Builder::phi_edge_value_ref(
		zend_mir_value_id value_id, uint32_t predecessor_block) {
	const int32_t value_index = zend_tpde_value_index(plan_, value_id);
	const int32_t definition = value_index < 0
			|| plan_->value_definition_instructions == nullptr
		? -1
		: plan_->value_definition_instructions[value_index];
	if (definition >= 0
			&& static_cast<uint32_t>(definition)
				< selected_phi_results.size()) {
		const IRValueRef selected = selected_phi_results[
			static_cast<uint32_t>(definition)];
		if (selected != INVALID_VALUE_REF
				&& machine_block_dominates(
					instruction_blocks[definition], predecessor_block)) {
			return selected;
		}
	}
	return value_ref(value_id);
}

void ZendIRAdaptor::Builder::resolve_phi_edges() {
	entry_register_definitions.assign(
		plan_->instruction_count, 0);
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (record.opcode != ZEND_MIR_OPCODE_PHI
				|| record.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan_, record.result_id);
		const IRValueRef canonical_result =
			result_index < 0 ? INVALID_VALUE_REF
				: IRValueRef{
					MIR_VALUE_BASE
						+ static_cast<uint32_t>(result_index)};
		const zend_mir_storage_id storage_id =
			canonical_result == INVALID_VALUE_REF
				? ZEND_MIR_ID_INVALID
				: canonical_storage(canonical_result);
		IRValueRef selected_phi = selected_phi_results[i];
		const zend_tpde_machine_value_kind phi_machine_kind =
			selected_phi != INVALID_VALUE_REF
				? machine_kind(selected_phi)
			: canonical_result == INVALID_VALUE_REF
				? ZEND_TPDE_MACHINE_VALUE_I64
				: machine_kind(canonical_result);
		const bool pointer_phi =
			(selected_phi != INVALID_VALUE_REF
				&& zend_tpde_machine_kind_is_pointer(phi_machine_kind))
			|| (selected_phi == INVALID_VALUE_REF
				&& canonical_result != INVALID_VALUE_REF
				&& zend_tpde_machine_kind_is_pointer(phi_machine_kind)
				&& machine_value_is_register_authoritative(
					canonical_result));
		const bool boxed_phi =
			!phi_uses_runtime_short_circuit_slot(instruction, storage_id)
			&& !std::binary_search(
				frame_authoritative_phi_storages.begin(),
				frame_authoritative_phi_storages.end(), storage_id)
			&& ((selected_phi != INVALID_VALUE_REF
				&& machine_kind(selected_phi)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
			|| (canonical_result != INVALID_VALUE_REF
				&& std::binary_search(
					lazy_mutation_storages.begin(),
					lazy_mutation_storages.end(), storage_id)));
		if (!zend_mir_id_is_valid(storage_id)
				|| (selected_phi == INVALID_VALUE_REF && result_index >= 0
					&& machine_value_used[
						static_cast<uint32_t>(result_index)] == 0)
				|| (!pointer_phi && !boxed_phi)) {
			continue;
		}
		const int32_t source_block = block_index(record.block_id);
		if (source_block < 0) {
			valid_ = false;
			continue;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[source_block];
		const uint32_t predecessor_count =
			plan_->block_predecessor_offsets[source_block + 1]
				- predecessor_begin;
		if (predecessor_count != instruction.operand_count
				|| instruction.operand_offset
					> plan_->instruction_operand_count
				|| instruction.operand_count
					> plan_->instruction_operand_count
						- instruction.operand_offset) {
			valid_ = false;
			continue;
		}

		std::vector<IRValueRef> inputs(
			predecessor_count, INVALID_VALUE_REF);
		std::vector<IRValueRef> cold_inputs(
			predecessor_count, INVALID_VALUE_REF);
		std::vector<PendingPhiInput> conversions;
		bool supported = true;
		for (uint32_t n = 0; n < predecessor_count; ++n) {
			const uint32_t predecessor =
				plan_->block_predecessors[predecessor_begin + n];
			IRValueRef input = predecessor < plan_->block_count
				? phi_edge_value_ref(
					zend_tpde_operand_at(plan_, &instruction, n),
					final_blocks[predecessor])
				: INVALID_VALUE_REF;
			const int32_t input_index = zend_tpde_value_index(
				plan_, zend_tpde_operand_at(plan_, &instruction, n));
			if (input_index >= 0) {
				const IRValueRef materializable =
					resolve_materializable_scalar(
						static_cast<uint32_t>(input_index),
						final_blocks[predecessor], UINT32_MAX, 0);
				if (materializable != INVALID_VALUE_REF) {
					input = materializable;
				}
			}
			if (input == INVALID_VALUE_REF
					|| predecessor >= plan_->block_count) {
				supported = false;
				break;
			}
			if (machine_value_is_register_authoritative(input)
					&& machine_kind(input)
						== (pointer_phi
							? phi_machine_kind
							: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
					&& machine_value_has_register_definition(input)) {
				inputs[n] = input;
				cold_inputs[n] = input;
				continue;
			}

			InstKind conversion_kind;
			IRValueRef conversion_source = input;
			const zend_mir_scalar_type_mask input_type =
				exact_type(input);
			uint64_t constant_bits;
			const bool scalar_input =
				(input_type == ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_BOOL)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_F64);
			if (!pointer_phi && scalar_input
					&& ((machine_value_is_register_authoritative(input)
							&& (machine_value_has_register_definition(input)
								|| entry_argument_has_register_definition(input)))
						|| constant(input, &constant_bits))) {
				conversion_kind = InstKind::BoxScalar;
			} else {
				const uint32_t reference = machine_reference_index(
					ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
					storage_id);
				if (canonical_storage(input) != storage_id
						|| reference == UINT32_MAX) {
					supported = false;
					break;
				}
				conversion_source = add_derived_value(
					ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
					ZEND_MIR_SCALAR_TYPE_NONE, storage_id,
					false, 0, UINT8_MAX,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN, reference);
				conversion_kind = InstKind::ZvalPayloadLoad;
			}
			if (conversion_source == INVALID_VALUE_REF) {
				supported = false;
				break;
			}
			const IRValueRef boxed = add_derived_value(
				pointer_phi
					? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
					: ZEND_MIR_REPRESENTATION_ZVAL,
				input_type,
				storage_id, false, 0,
				pointer_phi
					? phi_machine_kind
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ownership(input), refcount_state(input));
			inputs[n] = boxed;
			cold_inputs[n] = boxed;
			conversions.push_back({
				conversion_kind, final_blocks[predecessor],
				conversion_source, boxed,
				storage_id, input_type, false});
			const uint32_t cold_predecessor =
				boxed_cond_cold_by_predecessor[predecessor];
			if (cold_predecessor != UINT32_MAX) {
					const IRValueRef cold_boxed = add_derived_value(
						pointer_phi
							? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
							: ZEND_MIR_REPRESENTATION_ZVAL,
					input_type,
					storage_id, false, 0,
					pointer_phi
						? phi_machine_kind
						: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
					ownership(input), refcount_state(input));
				cold_inputs[n] = cold_boxed;
				conversions.push_back({
					conversion_kind, cold_predecessor,
					conversion_source, cold_boxed,
					storage_id, input_type, false});
			}
		}
		if (!supported) {
			continue;
		}
		if (selected_phi == INVALID_VALUE_REF) {
			selected_phi = add_derived_value(
				pointer_phi
					? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
					: ZEND_MIR_REPRESENTATION_ZVAL,
				pointer_phi
					? exact_type(canonical_result)
					: ZEND_MIR_SCALAR_TYPE_NONE,
				storage_id, false, 0,
				pointer_phi
					? phi_machine_kind
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				pointer_phi
					? ZEND_MIR_OWNERSHIP_STATE_BORROWED
					: ownership(canonical_result),
				pointer_phi
					? ZEND_MIR_REFCOUNT_UNKNOWN
					: refcount_state(canonical_result));
			if (result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= active_value_overrides().size()) {
				valid_ = false;
				continue;
			}
			selected_phi_results[i] = selected_phi;
			selected_phi_definitions.add(i, selected_phi);
		}
		for (uint32_t n = 0; n < predecessor_count; ++n) {
			boxed_phi_input_overrides[
				instruction.operand_offset + n] = inputs[n];
			boxed_phi_cold_input_overrides[
				instruction.operand_offset + n] = cold_inputs[n];
		}
		pending_phi_inputs.insert(pending_phi_inputs.end(),
			conversions.begin(), conversions.end());
	}
}

void ZendIRAdaptor::Builder::load_slot_backed_phi_inputs() {
	/*
	 * An exact scalar stays slot-backed when its defining opcode produced
	 * no register value. TPDE PHIs need a machine definition on every
	 * incoming edge, so such payloads are loaded in their predecessor
	 * blocks.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		if (record.opcode != ZEND_MIR_OPCODE_PHI) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan_, record.result_id);
		const IRValueRef result = result_index < 0
			? INVALID_VALUE_REF
			: value_ref(record.result_id);
		if (result == INVALID_VALUE_REF
				|| representation(result)
					== ZEND_MIR_REPRESENTATION_ZVAL
				|| zend_tpde_machine_kind_is_pointer(machine_kind(result))
				|| !zend_mir_scalar_type_is_exact(exact_type(result))
				|| exact_type(result) == ZEND_MIR_SCALAR_TYPE_NULL
				|| !machine_value_has_result_representation(result)) {
			continue;
		}
		const int32_t source_block = block_index(record.block_id);
		if (source_block < 0) {
			valid_ = false;
			continue;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[source_block];
		const uint32_t predecessor_count =
			plan_->block_predecessor_offsets[source_block + 1]
				- predecessor_begin;
		if (predecessor_count != instruction.operand_count) {
			valid_ = false;
			continue;
		}
		for (uint32_t n = 0; n < predecessor_count; ++n) {
			const uint32_t operand_index = instruction.operand_offset + n;
			IRValueRef input = value_ref(
				zend_tpde_operand_at(plan_, &instruction, n));
			const int32_t input_index = zend_tpde_value_index(
				plan_, zend_tpde_operand_at(plan_, &instruction, n));
			const bool registerless_source = input_index >= 0
				&& (plan_->value_definition_instructions == nullptr
					|| plan_->value_definition_instructions[input_index] < 0);
			if (input_index >= 0) {
				const IRValueRef materializable =
					resolve_materializable_scalar(
						static_cast<uint32_t>(input_index),
						final_blocks[plan_->block_predecessors[
							predecessor_begin + n]], UINT32_MAX, 0);
				if (materializable != INVALID_VALUE_REF) {
					input = materializable;
				}
			}
			uint64_t constant_bits;
			if (input == INVALID_VALUE_REF
					|| constant(input, &constant_bits)
					|| machine_value_has_register_definition(input)) {
				continue;
			}
			const bool matching_scalar =
				exact_type(input) == exact_type(result)
				&& machine_kind(input) == machine_kind(result);
			const bool canonical_boxed =
				representation(input) == ZEND_MIR_REPRESENTATION_ZVAL
				&& machine_kind(input)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& canonical_storage(input)
					== canonical_storage(result);
			if ((!matching_scalar && !canonical_boxed)
					|| (!registerless_source && !canonical_boxed)) {
				continue;
			}
			const zend_mir_storage_id storage_id =
				canonical_storage(input);
			const IRValueRef address = frame_slot_address(storage_id);
			const IRValueRef loaded = address != INVALID_VALUE_REF
				? add_derived_value(
					representation(result), exact_type(result), storage_id,
					false, 0, machine_kind(result),
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN)
				: INVALID_VALUE_REF;
			const uint32_t predecessor =
				plan_->block_predecessors[predecessor_begin + n];
			if (loaded == INVALID_VALUE_REF
					|| predecessor >= plan_->block_count) {
				continue;
			}
			boxed_phi_input_overrides[operand_index] = loaded;
			pending_phi_inputs.push_back({
				InstKind::ZvalPayloadLoad, final_blocks[predecessor],
				address, loaded, storage_id, exact_type(result), false});
			const uint32_t cold_predecessor =
				boxed_cond_cold_by_predecessor[predecessor];
			if (cold_predecessor != UINT32_MAX) {
				const IRValueRef cold_loaded = add_derived_value(
					representation(result), exact_type(result), storage_id,
					false, 0, machine_kind(result),
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN);
				boxed_phi_cold_input_overrides[operand_index] = cold_loaded;
				pending_phi_inputs.push_back({
					InstKind::ZvalPayloadLoad, cold_predecessor,
					address, cold_loaded, storage_id,
					exact_type(result), false});
			}
		}
	}
}

void ZendIRAdaptor::Builder::merge_short_circuit_results() {
	/*
	 * Source short-circuit temporaries have no MIR PHI record. A frozen
	 * register merge names the two exact boolean definitions reaching the
	 * terminal source branch; their merge becomes a TPDE PHI, so the branch
	 * never reads the unmaterialized slot.
	 */
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[i];
		if (!machine_block_reachable(instruction_blocks[i])) {
			continue;
		}
		const zend_mir_instruction_record record =
			instruction_record_at(i);
		const int32_t result_index =
			instruction.source_op1_binding.value_index;
		if ((instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE) == 0
				|| !valid_value_index(result_index)
				|| !zend_mir_id_is_valid(
					instruction.value_operation.op1_storage_id)) {
			continue;
		}
		const int32_t source_block = block_index(record.block_id);
		if (source_block < 0) {
			valid_ = false;
			continue;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[
				static_cast<uint32_t>(source_block)];
		const uint32_t predecessor_end =
			plan_->block_predecessor_offsets[
				static_cast<uint32_t>(source_block) + 1];
		if (predecessor_end - predecessor_begin != 2) {
			valid_ = false;
			continue;
		}
		std::array<IRValueRef, 2> inputs{
			INVALID_VALUE_REF, INVALID_VALUE_REF};
		bool supported = true;
		for (uint32_t n = 0; n < inputs.size(); ++n) {
			const uint32_t candidate_plus_one =
				instruction.register_merge_producer_plus_one[n];
			if (candidate_plus_one == 0
					|| candidate_plus_one > plan_->instruction_count) {
				supported = false;
				break;
			}
			const uint32_t candidate = candidate_plus_one - 1;
			const zend_tpde_instruction &producer =
				plan_->instructions[candidate];
			const zend_mir_instruction_record producer_record =
				instruction_record_at(candidate);
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
				&& register_boolean_results[candidate]
					!= INVALID_VALUE_REF;
			const IRValueRef selected = register_edge || register_result
				? active_instruction_results()[candidate]
				: INVALID_VALUE_REF;
			if (selected != INVALID_VALUE_REF
					&& exact_type(selected)
						== ZEND_MIR_SCALAR_TYPE_I1
					&& machine_kind(selected)
						== ZEND_TPDE_MACHINE_VALUE_BOOL
					&& machine_value_has_register_definition(selected)) {
				inputs[n] = selected;
			}
			if (inputs[n] == INVALID_VALUE_REF) {
				supported = false;
				break;
			}
		}
		if (!supported) {
			valid_ = false;
			continue;
		}
		const IRValueRef merged = add_derived_value(
			ZEND_MIR_REPRESENTATION_I1,
			ZEND_MIR_SCALAR_TYPE_I1,
			instruction.value_operation.op1_storage_id,
			false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_IMMORTAL);
		active_value_overrides()[
			static_cast<uint32_t>(result_index)] = merged;
		block_phis.push_back({instruction_blocks[i], merged});
		phi_values_[static_cast<uint32_t>(merged)] = 1;
		Slice &input_slice =
			phi_input_slices_[static_cast<uint32_t>(merged)];
		input_slice.offset =
			static_cast<uint32_t>(phi_inputs_.size());
		for (uint32_t n = 0; n < inputs.size(); ++n) {
			const uint32_t predecessor =
				plan_->block_predecessors[predecessor_begin + n];
			phi_inputs_.push_back(
				{inputs[n], IRBlockRef{final_blocks[predecessor]}});
			++input_slice.count;
		}
	}
}

void ZendIRAdaptor::Builder::order_generator_resumes() {
	/* The resume indices in ascending order of their target and of
	 * their landing source position; equal positions keep index order. */
	resumes_by_target.resize(plan_->generator_resume_count);
	for (uint32_t index = 0; index < resumes_by_target.size(); ++index) {
		resumes_by_target[index] = index;
	}
	resumes_by_landing = resumes_by_target;
	std::ranges::stable_sort(resumes_by_target, {},
		[&](uint32_t resume) {
			return plan_->generator_resume_targets[resume];
		});
	std::ranges::stable_sort(resumes_by_landing, {},
		[&](uint32_t resume) {
			return plan_->generator_resume_landings[resume];
		});
}

void ZendIRAdaptor::Builder::emit_generator_resume(
		uint32_t block, uint32_t source_position, bool exact_source_position,
		uint32_t use_instruction) {
	const uint32_t *positions = exact_source_position
		? plan_->generator_resume_targets
		: plan_->generator_resume_landings;
	for (const uint32_t resume_index : std::ranges::equal_range(
			exact_source_position
				? resumes_by_target : resumes_by_landing,
			source_position, {},
			[&](uint32_t resume) { return positions[resume]; })) {
		if (generator_resume_emitted[resume_index] != 0) {
			continue;
		}
		generator_resume_emitted[resume_index] = 1;
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(IRValueRef{FRAME_VALUE});
		const uint32_t resume_value_offset =
			static_cast<uint32_t>(generator_resume_values_.size());
		const auto &source_overrides =
			active_source_ssa_overrides();
		zend_tpde_visit_generator_resume_values(plan_, resume_index,
				[&](uint32_t value) {
			const zend_tpde_value &plan_value = plan_->values[value];
			IRValueRef candidate = value_ref(plan_value.id);
			if (zend_mir_value_is_original_ssa(plan_value.id)
					&& plan_value.id < source_overrides.size()
					&& source_overrides[plan_value.id]
						!= INVALID_VALUE_REF) {
				candidate = source_overrides[plan_value.id];
			}
			uint64_t constant_bits = 0;
			if (candidate == INVALID_VALUE_REF
					|| !zend_mir_id_is_valid(
						canonical_storage(candidate))
					|| constant(candidate, &constant_bits)
					|| !machine_value_is_register_authoritative(candidate)) {
				return true;
			}
			auto add_resume_value = [&](IRValueRef resumed) {
				const uint32_t raw = static_cast<uint32_t>(resumed);
				if (raw >= resume_value_stamps.size()) {
					resume_value_stamps.resize(raw + 1, 0);
				}
				if (resume_value_stamps[raw] == resume_index + 1) {
					return;
				}
				resume_value_stamps[raw] = resume_index + 1;
				generator_resume_values_.push_back(resumed);
			};
			add_resume_value(candidate);
			/*
			 * An elided COPY leaves later uses of the live value
			 * (statepoint stores, PHI edges, copy operands) on the
			 * register of its input. Resume runs in a new native
			 * activation, so that register must be redefined from
			 * the same slot as well; the input holds the same
			 * scalar by SSA.
			 */
			const IRValueRef resolved = resolve_materializable_scalar(
				value, block, use_instruction, 0);
			if (resolved != INVALID_VALUE_REF
					&& resolved != candidate
					&& canonical_storage(resolved)
						== canonical_storage(candidate)
					&& machine_kind(resolved) == machine_kind(candidate)
					&& !constant(resolved, &constant_bits)
					&& machine_value_has_register_definition(resolved)
					&& machine_value_is_register_authoritative(
						resolved)) {
				add_resume_value(resolved);
			}
			return true;
		});
		InstNode resume_node{
			.kind = InstKind::GeneratorResume,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = resume_index,
			.result = INVALID_VALUE_REF,
			.operand_offset = operand_offset,
			.operand_count =
				static_cast<uint32_t>(operands_.size()) - operand_offset,
			.has_result = false};
		resume_node.generator_resume_value_offset = resume_value_offset;
		resume_node.generator_resume_value_count =
			static_cast<uint32_t>(generator_resume_values_.size())
				- resume_value_offset;
		add_node(block_instructions, block, std::move(resume_node));
	}
}

void ZendIRAdaptor::Builder::index_call_fragments() {
	for (uint32_t instruction_index = 0;
			instruction_index < plan_->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan_->instructions[instruction_index];
		if (!instruction.user_opcode_call_fragments
				|| phased_source_call(instruction_index)
				|| instruction.user_call == nullptr) {
			continue;
		}
		if (call_fragment_instructions.empty()) {
			call_fragment_instructions.assign(
				plan_->source_opcode_count, UINT32_MAX);
		}
		auto claim = [&](uint32_t position) {
			if (position < call_fragment_instructions.size()
					&& call_fragment_instructions[position]
						== UINT32_MAX) {
				call_fragment_instructions[position] = instruction_index;
			}
		};
		const zend_native_user_call_descriptor *descriptor =
			instruction.user_call;
		claim(descriptor->init_source_position);
		claim(descriptor->do_source_position);
		for (uint32_t argument = 0;
				argument < descriptor->argument_count; ++argument) {
			claim(descriptor->arguments[argument].source_position);
		}
	}
}

void ZendIRAdaptor::Builder::emit_source_landing(
		uint32_t block, uint32_t source_position, uint32_t use_instruction) {
	if (source_position >= source_landing_emitted.size()
			|| source_landing_emitted[source_position] != 0
			|| source_landing_blocks[source_position] != block) {
		return;
	}
	source_landing_emitted[source_position] = 1;
	/*
	 * A generator continuation names the next Zend source opcode,
	 * which may be implemented by a source call fragment preceding
	 * the next MIR record. Place the machine landing before that
	 * fragment so resume executes the complete source operation.
	 */
	emit_generator_resume(
		block, source_position, true, use_instruction);
	if (plan_->user_opcode_callbacks) {
		add_node(block_instructions, block, InstNode{
			.kind = InstKind::UserOpcodeLanding,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = source_position,
			.result = INVALID_VALUE_REF,
			.operand_offset = 0,
			.operand_count = 0,
			.has_result = false});
		if (zend_get_user_opcode_handler(
				plan_->source_opcodes[source_position].opcode)
				!= nullptr) {
			const uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
			operands_.push_back(IRValueRef{FRAME_VALUE});
			operands_.push_back(IRValueRef{FRAME_VALUE});
			for (size_t dispatch_source = 0;
					dispatch_source
						< user_opcode_dispatch_to_sources_.size();
					++dispatch_source) {
				for (uint32_t target = 0;
						target < plan_->user_opcode_target_count;
						++target) {
					for (uint32_t use = 0;
							use < zend_tpde_user_opcode_target_frame_uses(
								plan_->user_opcode_targets[target].kind);
							++use) {
						operands_.push_back(IRValueRef{FRAME_VALUE});
					}
				}
			}
			add_node(block_instructions, block, InstNode{
				.kind = InstKind::UserOpcodeGateway,
				.mir_instruction_index = UINT32_MAX,
				.argument_index = source_position,
				.result = INVALID_VALUE_REF,
				.operand_offset = operand_offset,
				.operand_count = static_cast<uint32_t>(
					operands_.size() - operand_offset),
				.has_result = false});
		}
		add_node(block_instructions, block, InstNode{
			.kind = InstKind::UserOpcodeDispatch,
			.mir_instruction_index = UINT32_MAX,
			.argument_index = source_position,
			.result = INVALID_VALUE_REF,
			.operand_offset = 0,
			.operand_count = 0,
			.has_result = false});
	}
	if (const zend_tpde_source_call_phase_entry *phase =
			zend_tpde_source_call_phase_at(plan_, source_position)) {
			auto append_phase = [&](InstKind kind,
					uint32_t argument_index,
					IRValueRef operand = INVALID_VALUE_REF,
					IRValueRef result = INVALID_VALUE_REF,
					bool has_result = false) {
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
				if (operand != INVALID_VALUE_REF) {
					operands_.push_back(operand);
				}
				InstNode node{
					.kind = kind,
					.mir_instruction_index = phase->instruction_index,
					.argument_index = argument_index,
					.result = result,
					.operand_offset = operand_offset,
					.operand_count = operand == INVALID_VALUE_REF ? 2u : 3u,
					.has_result = has_result};
				node.source_position = source_position;
				add_node(block_instructions, block, std::move(node));
			};
			if ((phase->phases
					& ZEND_TPDE_SOURCE_CALL_PHASE_INIT) != 0) {
				append_phase(
					InstKind::UserCallInit, UINT32_MAX);
			}
			if ((phase->phases
					& ZEND_TPDE_SOURCE_CALL_PHASE_SEND) != 0) {
				IRValueRef operand = INVALID_VALUE_REF;
				if ((phase->operand_flags
						& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE)
						!= 0) {
					if (phase->instruction_index
							>= plan_->instruction_count
							|| plan_->call_argument_bindings == nullptr) {
						valid_ = false;
						return;
					}
					const zend_tpde_instruction &instruction =
						plan_->instructions[phase->instruction_index];
					if (phase->argument_index
							>= instruction.call_argument_count) {
						valid_ = false;
						return;
					}
					const uint32_t binding_index =
						instruction.call_argument_offset
						+ phase->argument_index;
					if (binding_index < instruction.call_argument_offset
							|| binding_index
								>= plan_->call_argument_count) {
						valid_ = false;
						return;
					}
					const zend_tpde_source_value_binding &binding =
						plan_->call_argument_bindings[binding_index];
					if (!valid_value_index(binding.value_index)) {
						valid_ = false;
						return;
					}
					const zend_mir_scalar_type_mask exact_type =
						plan_->values[static_cast<uint32_t>(
							binding.value_index)].exact_type;
					operand = source_binding_value_ref(binding);
					if (operand == INVALID_VALUE_REF
							|| !zend_mir_scalar_type_is_exact(exact_type)
							|| (exact_type != ZEND_MIR_SCALAR_TYPE_I1
								&& exact_type != ZEND_MIR_SCALAR_TYPE_I64
								&& exact_type != ZEND_MIR_SCALAR_TYPE_F64)) {
						valid_ = false;
						return;
					}
				}
				append_phase(InstKind::UserCallSend,
					phase->argument_index, operand);
			}
			if ((phase->phases
					& ZEND_TPDE_SOURCE_CALL_PHASE_CHECK) != 0) {
				append_phase(
					InstKind::UserCallCheck, phase->argument_index);
			}
			if ((phase->phases
					& ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND) != 0) {
				append_phase(
					InstKind::UserCallExpand, UINT32_MAX);
			}
			if ((phase->phases
					& ZEND_TPDE_SOURCE_CALL_PHASE_DO) != 0) {
				const zend_mir_instruction_record record =
					instruction_record_at(phase->instruction_index);
				const IRValueRef result = value_ref(record.result_id);
				const int32_t result_index = zend_tpde_value_index(
					plan_, record.result_id);
				const bool machine_result =
					machine_value_has_result_representation(result)
					&& result_index >= 0
					&& machine_value_used[
						static_cast<uint32_t>(result_index)] != 0;
				append_phase(InstKind::UserCallDo, UINT32_MAX,
					INVALID_VALUE_REF, result, machine_result);
			}
	}
	if (source_position < call_fragment_instructions.size()
			&& call_fragment_instructions[source_position]
				!= UINT32_MAX) {
		const uint32_t instruction_index =
			call_fragment_instructions[source_position];
		const zend_native_user_call_descriptor *descriptor =
			plan_->instructions[instruction_index].user_call;
		const bool finish =
			source_position == descriptor->do_source_position;
		const zend_mir_instruction_record record =
			instruction_record_at(instruction_index);
		const IRValueRef result = finish
			? value_ref(record.result_id) : INVALID_VALUE_REF;
		const int32_t result_index = finish
			? zend_tpde_value_index(plan_, record.result_id) : -1;
		const bool machine_result = result != INVALID_VALUE_REF
			&& result_index >= 0
			&& zend_mir_scalar_type_is_exact(exact_type(result))
			&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL
			&& machine_value_used[
				static_cast<uint32_t>(result_index)] != 0;
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(IRValueRef{FRAME_VALUE});
		add_node(block_instructions, block, InstNode{
			.kind = InstKind::UserOpcodeCallFragment,
			.mir_instruction_index = instruction_index,
			.argument_index = source_position,
			.result = result,
			.operand_offset = operand_offset,
			.operand_count = 1,
			.has_result = machine_result});
	}
}

void ZendIRAdaptor::Builder::index_pending_phi_inputs() {
	/* The pending inputs by block, in insertion order: all of them are
	 * recorded before the instruction stream is built. */
	pending_by_block.resize(pending_phi_inputs.size());
	for (uint32_t index = 0; index < pending_by_block.size(); ++index) {
		pending_by_block[index] = index;
	}
	std::ranges::stable_sort(pending_by_block, {},
		[&](uint32_t index) { return pending_phi_inputs[index].block; });
}

std::ranges::subrange<std::vector<uint32_t>::iterator>
ZendIRAdaptor::Builder::block_pending(uint32_t block) {
	return std::ranges::equal_range(pending_by_block, block, {},
		[&](uint32_t index) {
			return pending_phi_inputs[index].block;
		});
}

void ZendIRAdaptor::Builder::emit_pending_phi_inputs(
		uint32_t block, uint32_t terminator_instruction) {
	for (const uint32_t pending_index : block_pending(block)) {
		PendingPhiInput &pending = pending_phi_inputs[pending_index];
		if (pending.emitted) {
			continue;
		}
		const uint32_t operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(pending.source);
		add_node(block_instructions, block, InstNode{
			.kind = pending.kind,
			.mir_instruction_index = terminator_instruction,
			.argument_index = UINT32_MAX,
			.result = pending.result,
			.operand_offset = operand_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = pending.storage_id,
			.exact_type = pending.exact_type,
			.synthetic = true});
		pending.emitted = true;
	}
}

bool ZendIRAdaptor::Builder::has_pending_phi_inputs(uint32_t block) {
	return std::ranges::any_of(block_pending(block),
		[&](uint32_t pending_index) {
			return !pending_phi_inputs[pending_index].emitted;
		});
}

void ZendIRAdaptor::Builder::elide_empty_silence_pairs() {
	elided_silence_instructions.assign(
		plan_->instruction_count, 0);
	/*
	 * Constant-folded source expressions can leave an adjacent
	 * BEGIN_SILENCE/END_SILENCE pair with nothing between them. Saving and
	 * restoring error_reporting is then an identity, and two runtime calls
	 * per pair push large generated functions past the target section
	 * limit. Only closed pairs are removed whose saved value has no other
	 * consumer and whose source positions no opcode callback, source-call
	 * fragment, debug probe or generator continuation observes.
	 */
	if (!source_landings
			&& plan_->generator_resume_count == 0
			&& plan_->source_opcodes != nullptr
			&& plan_->value_consumer_offsets != nullptr
			&& plan_->value_consumers != nullptr) {
		for (uint32_t i = 0; i + 1 < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &begin = plan_->instructions[i];
			const zend_tpde_instruction &end = plan_->instructions[i + 1];
			const zend_mir_instruction_record begin_record =
				instruction_record_at(i);
			const zend_mir_instruction_record end_record =
				instruction_record_at(i + 1);
			if (begin_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE
					|| end_record.opcode
						!= ZEND_MIR_OPCODE_VALUE_END_SILENCE
					|| instruction_blocks[i] == UINT32_MAX
					|| instruction_blocks[i] != instruction_blocks[i + 1]
					|| begin.debug_probe || end.debug_probe
					|| begin.user_opcode_call_fragments
					|| end.user_opcode_call_fragments
					|| begin_record.source_position_id == UINT32_MAX
					|| end_record.source_position_id
						!= begin_record.source_position_id + 1
					|| end_record.source_position_id
						>= plan_->source_opcode_count
					|| plan_->source_opcodes[
						begin_record.source_position_id].opcode
						!= ZEND_BEGIN_SILENCE
					|| plan_->source_opcodes[
						end_record.source_position_id].opcode
						!= ZEND_END_SILENCE) {
				continue;
			}
			const int32_t saved_value =
				begin.source_result_binding.value_index;
			if (!valid_value_index(saved_value)
					|| end.source_op1_binding.value_index != saved_value) {
				continue;
			}
			const uint32_t consumer_begin =
				plan_->value_consumer_offsets[
					static_cast<uint32_t>(saved_value)];
			const uint32_t consumer_end =
				plan_->value_consumer_offsets[
					static_cast<uint32_t>(saved_value) + 1];
			bool closed_pair = consumer_begin != consumer_end;
			for (uint32_t use = consumer_begin;
					closed_pair && use < consumer_end; ++use) {
				closed_pair = plan_->value_consumers[use].instruction_index
					== i + 1;
			}
			if (!closed_pair) {
				continue;
			}
			elided_silence_instructions[i] = 1;
			elided_silence_instructions[i + 1] = 1;
			++i;
		}
	}
}

void ZendIRAdaptor::Builder::record_source_producer(
		uint32_t instruction_index) {
	if (instruction_index >= active_instruction_results().size()) {
		return;
	}
	const IRValueRef result =
		active_instruction_results()[instruction_index];
	if (result == INVALID_VALUE_REF) {
		return;
	}
	const uint32_t value_index = static_cast<uint32_t>(result);
	if (value_index >= latest_source_producer_by_ir_value.size()) {
		latest_source_producer_by_ir_value.resize(value_index + 1, -1);
	}
	latest_source_producer_by_ir_value[value_index] =
		static_cast<int32_t>(instruction_index);
}

const std::vector<uint32_t> &ZendIRAdaptor::Builder::canonical_slot_writers(
		zend_mir_storage_id storage) {
	if (!slot_writers_built) {
		slot_writers_built = true;
		for (uint32_t index = 0; index < plan_->instruction_count;
				++index) {
			const zend_tpde_instruction &candidate =
				plan_->instructions[index];
			if (candidate.has_value_operation
					&& ((candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_BINARY_OP
						|| candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
						|| candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_INCDEC
						|| candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
						|| candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP)
						|| (candidate.record.opcode
								== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
							&& candidate.runtime_helper
								!= ZEND_NATIVE_HELPER_COUNT))) {
				slot_writers[candidate.value_operation.op1_storage_id]
					.push_back(index);
			}
		}
	}
	const auto found = slot_writers.find(storage);
	return found == slot_writers.end() ? no_slot_writers
		: found->second;
}

void ZendIRAdaptor::Builder::build_instructions() {
	for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
		build_instruction(i);
	}
}

void ZendIRAdaptor::Builder::build_instruction(uint32_t i) {
	if (i != 0) {
		record_source_producer(i - 1);
	}
	if (elided_silence_instructions[i] != 0) {
		return;
	}
	const zend_tpde_instruction &instruction = plan_->instructions[i];
	const zend_mir_instruction_record record =
		instruction_record_at(i);
	const bool boxed_cond_branch =
		is_boxed_cond_branch(instruction);
	if (boxed_cond_branches_.empty()) {
		boxed_cond_branches_.assign(
			plan_->instruction_count, BOXED_COND_BRANCH_UNKNOWN);
	}
	boxed_cond_branches_[i] = boxed_cond_branch ? 1 : 0;
	if (!zend_mir_id_is_valid(record.id)) {
		valid_ = false;
		return;
	}
	if (function_mode_ == FunctionMode::TypedBody
			&& (instruction.local_abi_transport
				|| zend_tpde_typed_body_frame_transport(
					plan_, instruction))) {
		return;
	}
	const uint32_t block = instruction_blocks[i];
	if (block == UINT32_MAX) {
		valid_ = false;
		return;
	}
	if (!machine_block_reachable(block)) {
		return;
	}
	InstContext ctx{
		.i = i,
		.instruction = instruction,
		.record = record,
		.boxed_cond_branch = boxed_cond_branch,
		.block = block};
	emit_instruction_landings(ctx);
	if ((instruction.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS) != 0) {
		return;
	}
	/* A proven operation in a typed body: register operands, a boxed
	 * number or boolean result and no slow edge. */
	if (function_mode_ == FunctionMode::TypedBody
			&& zend_tpde_typed_numeric_binary(plan_, instruction)) {
		lower_typed_body_operation(i, instruction, block);
		return;
	}
	/* A ternary's QM_ASSIGN forwards its register source. */
	if (function_mode_ == FunctionMode::TypedBody
			&& zend_tpde_typed_forward_assign(instruction)) {
		forward_typed_body_assign(i, instruction, block);
		return;
	}
	if (zend_mir_opcode_is_terminator(record.opcode)) {
		emit_pending_phi_inputs(block, i);
	}
	if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
			&& (instruction.user_opcode_call_fragments
				|| phased_source_call(i))) {
		return;
	}
	select_initial_result(ctx);
	select_register_operands(ctx);
	if (!select_component_call_result(ctx)) {
		return;
	}
	select_binary_result(ctx);
	if (!select_phi_result(ctx)) {
		return;
	}
	if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
		return;
	}
	if (record.opcode == ZEND_MIR_OPCODE_PHI) {
		lower_phi(ctx);
		return;
	}
	if (record.opcode == ZEND_MIR_OPCODE_COPY
			&& zend_mir_scalar_type_is_exact(exact_type(ctx.result))
			&& exact_type(ctx.result) != ZEND_MIR_SCALAR_TYPE_NULL) {
		const int32_t result_index =
			zend_tpde_value_index(plan_, record.result_id);
		if (result_index >= 0
				&& machine_value_used[
					static_cast<uint32_t>(result_index)] == 0) {
			return;
		}
	}

	select_source_scalar_result(ctx);
	if (!select_source_boxed_result(ctx)) {
		return;
	}
	select_internal_call_result(ctx);

	if (!select_machine_result(ctx)) {
		return;
	}
	if (!lower_boxed_copy(ctx)) {
		return;
	}
	ctx.operand_offset =
		static_cast<uint32_t>(operands_.size());
	ctx.typed_call_value_guards.clear();
	ctx.typed_call_owned_boxed_arguments.clear();
	ctx.assign_op_right_operand_index = UINT32_MAX;
	ctx.assign_op_left_operand_index = UINT32_MAX;
	ctx.packed_append_value_operand_index = UINT32_MAX;
	ctx.property_write_value_operand_index = UINT32_MAX;
	add_value_assign_operands(ctx);
	add_fetch_operands(ctx);
	add_compound_assign_operands(ctx);
	add_write_value_operands(ctx);
	add_register_unary_operands(ctx);
	add_slot_isset_operand(ctx);
	add_long_binary_operands(ctx);
	add_literal_reference_operand(ctx);
	add_data_operands(ctx);
	add_frame_and_call_operands(ctx);
	count_operands(ctx);
	add_boxed_boundary_operands(ctx);
	if (ctx.guarded_cold_block != UINT32_MAX) {
		lower_guarded_operation(ctx);
		return;
	}
	if (ctx.boxed_cond_cold_block != UINT32_MAX) {
		lower_boxed_cond_branch(ctx);
		return;
	}
	emit_instruction_node(ctx);
	publish_init_array_pointer(ctx);
	mark_selected_operation(ctx);
}

void ZendIRAdaptor::Builder::emit_instruction_landings(InstContext &ctx) {
	if (source_landings
			&& plan_->source_opcode_block_indices != nullptr
			&& ctx.record.source_position_id
				< plan_->source_opcode_count
			&& !(source_synthetic_entry
				&& ctx.record.block_id
					== plan_->function.entry_block_id)) {
		const uint32_t source_block =
			plan_->source_opcode_block_indices[
				ctx.record.source_position_id];
		if (source_block < source_block_next.size()) {
			const uint32_t source_end =
				plan_->source_block_ends[source_block];
			uint32_t &next_source =
				source_block_next[source_block];
			while (next_source != UINT32_MAX
					&& next_source <= ctx.record.source_position_id
					&& next_source < source_end) {
				emit_source_landing(
					static_cast<uint32_t>(ctx.block), next_source++, ctx.i);
			}
		}
	}
	emit_generator_resume(static_cast<uint32_t>(ctx.block),
		ctx.record.source_position_id, false, ctx.i);
}

void ZendIRAdaptor::Builder::select_initial_result(InstContext &ctx) {
	ctx.result = value_ref(ctx.record.result_id);
	if (ctx.i < transient_scalar_results.size()
			&& transient_scalar_results[ctx.i] != INVALID_VALUE_REF) {
		ctx.result = transient_scalar_results[ctx.i];
	}
	if (ctx.i < register_boolean_results.size()
			&& register_boolean_results[ctx.i] != INVALID_VALUE_REF) {
		ctx.result = register_boolean_results[ctx.i];
	}
	ctx.type_check_input = INVALID_VALUE_REF;
	IRValueRef type_check_result = INVALID_VALUE_REF;
	ctx.type_check_selection =
		function_mode_ == FunctionMode::TypedBody
				&& ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			? scalar_type_check_selection(
				plan_, ctx.instruction,
				&ctx.type_check_input, &type_check_result)
			: ScalarTypeCheckSelection::Invalid;
	if (ctx.type_check_selection
			!= ScalarTypeCheckSelection::Invalid) {
		const IRValueRef canonical_result = type_check_result;
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		if (type_check_result == INVALID_VALUE_REF
				|| exact_type(type_check_result)
					!= ZEND_MIR_SCALAR_TYPE_I1
				|| representation(type_check_result)
					!= ZEND_MIR_REPRESENTATION_I1) {
			const zend_mir_value_id result_ssa =
				ctx.instruction.value_operation.result
					.ssa_variable_id;
			if (result_ssa
					>= source_ssa_overrides_.size()) {
				valid_ = false;
			} else {
				type_check_result = add_derived_value(
					ZEND_MIR_REPRESENTATION_I1,
					ZEND_MIR_SCALAR_TYPE_I1,
					ctx.instruction.value_operation
						.result_storage_id);
				if (canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< value_overrides_.size()) {
					value_overrides_[
						canonical_index - MIR_VALUE_BASE] =
							type_check_result;
				}
				source_ssa_overrides_[result_ssa] =
					type_check_result;
			}
		}
		ctx.result = type_check_result;
	}
}

void ZendIRAdaptor::Builder::select_register_operands(InstContext &ctx) {
	ctx.register_condition = INVALID_VALUE_REF;
	ctx.register_cond_branch =
		is_register_cond_branch(ctx.instruction, &ctx.register_condition);
	ctx.canonical_bool_unary_exact_type =
		ZEND_MIR_SCALAR_TYPE_NONE;
	ctx.register_bool_unary_operand = INVALID_VALUE_REF;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
			&& ctx.instruction.has_value_operation
			&& (ctx.instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
			&& (ctx.instruction.value_operation.source_opcode == ZEND_BOOL
				|| ctx.instruction.value_operation.source_opcode
					== ZEND_BOOL_NOT)) {
		IRValueRef candidate = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if ((candidate == INVALID_VALUE_REF
				|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE)
				&& ctx.instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one != 0) {
			candidate = value_ref(zend_mir_value_from_original_ssa(
				ctx.instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one - 1));
		}
		if (candidate == INVALID_VALUE_REF) {
			candidate = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		if (candidate != INVALID_VALUE_REF
				&& exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_I1
				&& machine_value_is_register_authoritative(candidate)
				&& machine_value_has_register_definition(candidate)
				&& register_operand_reaches(candidate,
					static_cast<uint32_t>(ctx.block), ctx.i)
				&& machine_kind(candidate)
					!= ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
			ctx.canonical_bool_unary_exact_type =
				ZEND_MIR_SCALAR_TYPE_I1;
			ctx.register_bool_unary_operand = candidate;
		}
	}
	ctx.register_boxed_condition_operand = INVALID_VALUE_REF;
	ctx.register_string_condition_operand = INVALID_VALUE_REF;
	ctx.register_string_length_operand = INVALID_VALUE_REF;
	ctx.constant_string_length = false;
	ctx.constant_string_length_bits = 0;
	if (ctx.instruction.has_value_operation
			&& (ctx.boxed_cond_branch
				|| (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
					&& ctx.instruction.value_operation.source_opcode
						== ZEND_STRLEN))) {
		IRValueRef candidate = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if (candidate == INVALID_VALUE_REF) {
			candidate = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		if (candidate != INVALID_VALUE_REF
				&& machine_value_is_register_authoritative(candidate)
				&& machine_value_has_register_definition(candidate)
				&& register_operand_reaches(candidate,
					static_cast<uint32_t>(ctx.block), ctx.i)) {
			if (ctx.boxed_cond_branch
					&& machine_kind(candidate)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				ctx.register_boxed_condition_operand = candidate;
			} else if (ctx.boxed_cond_branch
					&& machine_kind(candidate)
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
				ctx.register_string_condition_operand = candidate;
			} else if (!ctx.boxed_cond_branch
					&& machine_kind(candidate)
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
				ctx.register_string_length_operand = candidate;
				ctx.constant_string_length = known_string_literal(
					candidate, &ctx.constant_string_length_bits, nullptr);
			}
		}
	}
}

bool ZendIRAdaptor::Builder::select_component_call_result(InstContext &ctx) {
	const bool typed_component_call =
		ctx.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& ctx.instruction.direct_call != nullptr
		&& frozen_typed_component_call(ctx.i);
	ctx.register_component_call =
		typed_component_call
			|| (ctx.record.opcode
				== ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& ctx.instruction.direct_call != nullptr
			&& frozen_effect_closed_inline(ctx.i));
	if (ctx.register_component_call) {
		if (ctx.i >= register_component_results.size()) {
			valid_ = false;
			return false;
		}
		if (register_component_results[ctx.i] != INVALID_VALUE_REF) {
			ctx.result = register_component_results[ctx.i];
		} else if (ctx.result == INVALID_VALUE_REF
				|| exact_type(ctx.result)
					!= ZEND_MIR_SCALAR_TYPE_NULL) {
			valid_ = false;
			return false;
		}
	}
	return true;
}

void ZendIRAdaptor::Builder::select_binary_result(InstContext &ctx) {
	if (ctx.i < register_binary_results.size()
			&& register_binary_results[ctx.i] != INVALID_VALUE_REF) {
		ctx.result = register_binary_results[ctx.i];
	}
	if (ctx.i < register_branch_results.size()
			&& register_branch_results[ctx.i] != INVALID_VALUE_REF) {
		ctx.result = register_branch_results[ctx.i];
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_BINARY_OP
			&& ctx.instruction.has_value_operation
			&& source_result_used[ctx.i] != 0
			&& !machine_value_has_result_representation(ctx.result)) {
		IRValueRef left = INVALID_VALUE_REF;
		IRValueRef right = INVALID_VALUE_REF;
		if (long_binary_machine_operands(ctx.instruction, left, right)) {
			const uint32_t source_opcode =
				ctx.instruction.value_operation.source_opcode;
			const bool boolean_result =
				source_opcode == ZEND_IS_IDENTICAL
				|| source_opcode == ZEND_IS_NOT_IDENTICAL
				|| source_opcode == ZEND_IS_EQUAL
				|| source_opcode == ZEND_IS_NOT_EQUAL
				|| source_opcode == ZEND_IS_SMALLER
				|| source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
			auto &source_overrides =
				active_source_ssa_overrides();
			const IRValueRef canonical_result =
				source_operand_value_ref(
					ctx.instruction.value_operation.result);
			const zend_mir_value_id result_ssa =
				ctx.instruction.value_operation.result
					.ssa_variable_id;
			ctx.result = add_derived_value(
				ZEND_MIR_REPRESENTATION_ZVAL,
				boolean_result
					? ZEND_MIR_SCALAR_TYPE_I1
					: ZEND_MIR_SCALAR_TYPE_I64,
				ctx.instruction.value_operation
					.result_storage_id,
				false, 0,
				ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
			set_instruction_result(ctx.i, ctx.result);
			if (result_ssa < source_overrides.size()) {
				source_overrides[result_ssa] = ctx.result;
			} else if (canonical_result
					== INVALID_VALUE_REF) {
				valid_ = false;
			}
		}
	}
}

bool ZendIRAdaptor::Builder::select_phi_result(InstContext &ctx) {
	if (ctx.record.opcode == ZEND_MIR_OPCODE_PHI) {
		const int32_t result_index =
			zend_tpde_value_index(plan_, ctx.record.result_id);
		if (result_index < 0
				|| static_cast<uint32_t>(result_index)
					>= active_value_overrides().size()) {
			valid_ = false;
			return false;
		}
		ctx.result = IRValueRef{
			MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
		if (ctx.i < selected_phi_results.size()
				&& selected_phi_results[ctx.i] != INVALID_VALUE_REF) {
			ctx.result = selected_phi_results[ctx.i];
			set_instruction_result(ctx.i, ctx.result);
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = ctx.result;
		}
	}
	return true;
}

void ZendIRAdaptor::Builder::select_source_scalar_result(InstContext &ctx) {
	zend_tpde_slot_isset_empty source_scalar_isset_layout{};
	const bool source_scalar_slot_isset =
		ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
		&& ctx.instruction.has_value_operation
		&& zend_tpde_slot_isset_empty_at(
			ctx.instruction, &source_scalar_isset_layout);
	const bool source_scalar_result_machine_eligible =
		function_mode_ == FunctionMode::ZendEntry
			&& ctx.instruction.has_value_operation
			&& source_result_used[ctx.i] != 0
			&& ((ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				&& source_scalar_slot_isset
				&& (ctx.instruction.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT)
						!= 0)
				|| (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
					&& (ctx.register_bool_unary_operand
							!= INVALID_VALUE_REF
						|| (ctx.instruction.value_operation.source_opcode
								== ZEND_STRLEN
							&& ctx.register_string_length_operand
								!= INVALID_VALUE_REF))));
	if (source_scalar_result_machine_eligible
			&& !machine_value_has_result_representation(ctx.result)) {
		auto &value_overrides = active_value_overrides();
		auto &source_overrides = active_source_ssa_overrides();
		const IRValueRef canonical_result =
			source_binding_value_ref(
				ctx.instruction.source_result_binding);
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		const zend_mir_scalar_type_mask result_type =
			ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
				&& ctx.instruction.value_operation.source_opcode == ZEND_STRLEN
				? ZEND_MIR_SCALAR_TYPE_I64
				: ZEND_MIR_SCALAR_TYPE_I1;
		const zend_mir_value_id result_ssa =
			ctx.instruction.value_operation.result.ssa_variable_id;
		const zend_mir_storage_id result_storage =
			ctx.instruction.value_operation.result_storage_id;
		if (canonical_result != INVALID_VALUE_REF
				&& is_mir_value(canonical_index)
				&& zend_mir_id_is_valid(result_storage)) {
			ctx.result = add_derived_value(
				zend_tpde_scalar_representation(result_type),
				result_type, result_storage,
				ctx.constant_string_length,
				ctx.constant_string_length
					? ctx.constant_string_length_bits : 0,
				zend_tpde_scalar_machine_kind(result_type),
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_IMMORTAL);
			set_instruction_result(ctx.i, ctx.result);
			value_overrides[
				canonical_index - MIR_VALUE_BASE] = ctx.result;
			if (result_ssa < source_overrides.size()) {
				source_overrides[result_ssa] = ctx.result;
			}
		}
	}
}

bool ZendIRAdaptor::Builder::select_source_boxed_result(InstContext &ctx) {
	bool source_boxed_result_machine_eligible = false;
	bool source_dynamic_direct_long = false;
	bool source_array_constant_key = false;
	const IRValueRef canonical_source_result =
		ctx.instruction.has_value_operation
			? source_binding_value_ref(
				ctx.instruction.source_result_binding)
			: INVALID_VALUE_REF;
	const int32_t direct_source_consumer =
		ctx.i < source_result_consumer.size()
			? source_result_consumer[ctx.i] : -1;
	IRValueRef source_array_key = INVALID_VALUE_REF;
	bool source_array_key_entry_register = false;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			&& ctx.instruction.has_value_operation) {
		source_array_key = source_binding_value_ref(
			ctx.instruction.source_op2_binding);
		if (source_array_key == INVALID_VALUE_REF) {
			source_array_key = source_operand_value_ref(
				ctx.instruction.value_operation.op2);
		}
		uint64_t key_constant_bits = 0;
		source_array_constant_key =
			ctx.instruction.value_operation.op2.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| (source_array_key != INVALID_VALUE_REF
				&& (known_string_literal(
					source_array_key, nullptr, nullptr)
				|| (machine_kind(source_array_key)
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					&& !zend_mir_id_is_valid(
						canonical_storage(source_array_key)))
				|| (exact_type(source_array_key)
						== ZEND_MIR_SCALAR_TYPE_I64
					&& constant(source_array_key,
						&key_constant_bits))));
		const uint32_t raw_key =
			static_cast<uint32_t>(source_array_key);
		if (function_mode_ == FunctionMode::ZendEntry
				&& is_mir_value(raw_key)
				&& plan_->values[raw_key - MIR_VALUE_BASE]
					.argument_index >= 0) {
			const TypedBodyAbiType key_abi = typed_body_value_abi(
				plan_, raw_key - MIR_VALUE_BASE);
			source_array_key_entry_register = key_abi.valid
				&& key_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
		}
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.instruction.has_value_operation
			&& source_result_used[ctx.i] != 0
			&& source_result_has_direct_consumer(ctx.i)) {
		const zend_tpde_machine_reference *reference = nullptr;
		if (ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R) {
			zend_tpde_array_read layout{};
			source_boxed_result_machine_eligible =
				zend_tpde_array_read_at(ctx.instruction, &layout,
					zend_tpde_linux_inline_forms)
				&& operation_machine_reference(ctx.i, &reference)
				&& reference != nullptr
				&& reference->kind
					== ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT
				&& (layout.container_literal
					|| zend_mir_id_is_valid(
						reference->base_value_id))
				&& zend_mir_id_is_valid(
					reference->index_value_id)
				&& reference->scale == sizeof(zval)
				&& reference->access_width == sizeof(zval)
				&& (source_array_constant_key
					|| (source_array_key != INVALID_VALUE_REF
						&& ((exact_type(source_array_key)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& representation(source_array_key)
							== ZEND_MIR_REPRESENTATION_I64
						&& machine_kind(source_array_key)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (machine_kind(source_array_key)
							== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						&& machine_value_is_register_authoritative(
							source_array_key))
					|| (representation(source_array_key)
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& machine_kind(source_array_key)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
						&& (machine_value_has_register_definition(
								source_array_key)
							|| source_array_key_entry_register)));
		} else if (ctx.record.opcode
				== ZEND_MIR_OPCODE_OBJECT_FETCH_R) {
			zend_tpde_object_property_read layout{};
			source_boxed_result_machine_eligible =
				zend_tpde_object_property_read_at(
					ctx.instruction, &layout)
				&& operation_machine_reference(ctx.i, &reference)
				&& reference != nullptr
				&& reference->kind
					== ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT
				&& reference->stable_storage_or_layout_id
					== layout.cache_offset
				&& reference->access_width == sizeof(zval);
		} else if (ctx.record.opcode
				== ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
			zend_tpde_dynamic_fetch_read layout{};
			source_boxed_result_machine_eligible =
				zend_tpde_dynamic_fetch_read_at(
					ctx.instruction, &layout);
			source_dynamic_direct_long =
				source_boxed_result_machine_eligible
					&& layout.direct_long;
		}
	}
	const bool unknown_reference_result =
		canonical_source_result != INVALID_VALUE_REF
		&& machine_kind(canonical_source_result)
			== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
		&& !zend_mir_scalar_type_is_exact(
			exact_type(canonical_source_result));
	const bool complete_boxed_source_result =
		(ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			&& source_array_constant_key)
		|| ctx.record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
		|| ctx.record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
		|| ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_COUNT;
	const bool direct_internal_argument_result =
		unknown_reference_result
		&& complete_boxed_source_result
		&& ctx.instruction.has_value_operation
		&& source_result_used[ctx.i] != 0
		&& direct_source_consumer > static_cast<int32_t>(ctx.i)
		&& static_cast<uint32_t>(direct_source_consumer)
			< plan_->instruction_count
		&& plan_->instructions[static_cast<uint32_t>(
			direct_source_consumer)].record.opcode
			== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL;
	const bool register_complete_array_result =
		ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
		&& source_array_constant_key
		&& source_result_used[ctx.i] != 0
		&& source_result_has_direct_consumer(ctx.i)
		&& direct_source_consumer > static_cast<int32_t>(ctx.i)
		&& static_cast<uint32_t>(direct_source_consumer)
			< plan_->instruction_count
		&& plan_->instructions[static_cast<uint32_t>(
			direct_source_consumer)].record.opcode
			== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL;
	/*
	 * The register result skips the result slot, which only a
	 * temporary may do: a CV result (opcache folds the ASSIGN into
	 * the fetch) is read from its slot later and owns its old value.
	 */
	const bool boxed_source_result =
		((ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				|| ctx.record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
				|| ctx.record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
			&& source_boxed_result_machine_eligible
			&& ctx.instruction.value_operation.result.slot_kind
				!= ZEND_MIR_SOURCE_SLOT_CV)
		|| direct_internal_argument_result
		|| register_complete_array_result;
	const bool boxed_helper_boundary_result =
		(ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			|| ctx.record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R)
		&& boxed_source_result
		&& direct_source_consumer > static_cast<int32_t>(ctx.i)
		&& static_cast<uint32_t>(direct_source_consumer)
			< plan_->instruction_count
		&& (plan_->instructions[static_cast<uint32_t>(
				direct_source_consumer)].record.opcode
				== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
			|| (plan_->instructions[static_cast<uint32_t>(
					direct_source_consumer)].record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& !frozen_typed_component_call(
					static_cast<uint32_t>(direct_source_consumer))));
	const bool typed_boxed_call_result =
		direct_source_consumer > static_cast<int32_t>(ctx.i)
		&& static_cast<uint32_t>(direct_source_consumer)
			< plan_->instruction_count
		&& frozen_typed_component_call(
			static_cast<uint32_t>(direct_source_consumer));
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.instruction.has_value_operation
			&& boxed_source_result
			&& (!machine_value_has_result_representation(ctx.result)
				|| unknown_reference_result
				|| register_complete_array_result
				|| (typed_boxed_call_result
					&& machine_kind(ctx.result)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& ownership(ctx.result)
						!= ZEND_MIR_OWNERSHIP_STATE_OWNED))) {
		auto &value_overrides = active_value_overrides();
		auto &source_overrides =
			active_source_ssa_overrides();
		const IRValueRef canonical_result = canonical_source_result;
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		const zend_mir_value_id result_ssa =
			ctx.instruction.value_operation.result
				.ssa_variable_id;
		const zend_mir_storage_id result_storage =
			ctx.instruction.value_operation.result_storage_id;
		if (canonical_result == INVALID_VALUE_REF
				|| !is_mir_value(canonical_index)
				|| !zend_mir_id_is_valid(result_storage)) {
			valid_ = false;
			return false;
		}
		/*
		 * Source value operations keep a void ZNMIR record, as their
		 * PHP semantics live in the executable descriptor, but the fast
		 * path or its cold continuation produces a real zval: its
		 * payload and type info form a two-part TPDE value, joined with
		 * the cold helper's result at the continuation PHI. An unknown
		 * source result would otherwise inherit the one-part
		 * reference-pointer carrier, which cannot snapshot the value
		 * when an optimized op array reuses the result TMP before a
		 * direct internal call runs.
		 */
		ctx.result = source_dynamic_direct_long
				&& !unknown_reference_result
				&& !register_complete_array_result
			? add_derived_value(
				ZEND_MIR_REPRESENTATION_I64,
				ZEND_MIR_SCALAR_TYPE_I64,
				result_storage, false, 0,
				ZEND_TPDE_MACHINE_VALUE_I64)
			: add_derived_value(
				ZEND_MIR_REPRESENTATION_ZVAL,
				exact_type(canonical_result),
				result_storage, false, 0,
				ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ZEND_MIR_OWNERSHIP_STATE_OWNED,
				ZEND_MIR_REFCOUNT_UNKNOWN);
		set_instruction_result(ctx.i, ctx.result);
		value_overrides[
			canonical_index - MIR_VALUE_BASE] = ctx.result;
		if (result_ssa < source_overrides.size()) {
			source_overrides[result_ssa] = ctx.result;
		}
		if (boxed_helper_boundary_result
				&& machine_kind(ctx.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			source_result_materialized_for_helper[ctx.i] = 1;
		}
	}
	return true;
}

void ZendIRAdaptor::Builder::select_internal_call_result(InstContext &ctx) {
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
			&& ctx.instruction.direct_internal_call != nullptr
			&& source_result_used[ctx.i] != 0
			&& !machine_value_has_result_representation(ctx.result)) {
		const zend_mir_scalar_type_mask result_type =
			ctx.instruction.direct_internal_call->result_type;
		const IRValueRef canonical_result = source_binding_value_ref(
			ctx.instruction.source_result_binding);
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		const zend_mir_storage_id result_storage =
			canonical_storage(canonical_result);
		const zend_mir_value_id result_ssa =
			ctx.instruction.direct_internal_call->result_operand
				.ssa_variable_id;
		if ((result_type == ZEND_MIR_SCALAR_TYPE_I1
				|| result_type == ZEND_MIR_SCALAR_TYPE_I64
				|| result_type == ZEND_MIR_SCALAR_TYPE_F64)
				&& canonical_result != INVALID_VALUE_REF
				&& is_mir_value(canonical_index)
				&& zend_mir_id_is_valid(result_storage)) {
			ctx.result = add_derived_value(
				zend_tpde_scalar_representation(result_type),
				result_type, result_storage, false, 0,
				zend_tpde_scalar_machine_kind(result_type),
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_IMMORTAL);
			set_instruction_result(ctx.i, ctx.result);
			active_value_overrides()[
				canonical_index - MIR_VALUE_BASE] = ctx.result;
			if (result_ssa < active_source_ssa_overrides().size()) {
				active_source_ssa_overrides()[result_ssa] = ctx.result;
			}
		}
	}
}

bool ZendIRAdaptor::Builder::select_machine_result(InstContext &ctx) {
	ctx.copy_input = INVALID_VALUE_REF;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_COPY
			&& ctx.record.representation
				== ZEND_MIR_REPRESENTATION_ZVAL
			&& ctx.result != INVALID_VALUE_REF
			&& ctx.instruction.operand_count == 1) {
		ctx.copy_input = resolve_copy_input(zend_tpde_operand_at(
			plan_, &ctx.instruction, 0), ctx.block, ctx.i);
	}
	if (ctx.i < mutation_results.size()
			&& mutation_results[ctx.i] != INVALID_VALUE_REF) {
		ctx.result = mutation_results[ctx.i];
	}
	ctx.machine_result =
		machine_value_needs_result_assignment(ctx.result);
	/*
	 * A register BOOL/BOOL_NOT operand or known string length helps
	 * only when the operation also publishes a machine result;
	 * canonical-only consumers need the result zval in the frame.
	 */
	if (!ctx.machine_result) {
		ctx.register_bool_unary_operand = INVALID_VALUE_REF;
		ctx.register_string_length_operand = INVALID_VALUE_REF;
		ctx.constant_string_length = false;
	}
	if (ctx.machine_result
			&& (ctx.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| ctx.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
			&& !ctx.register_component_call) {
		const int32_t result_index =
			zend_tpde_value_index(plan_, ctx.record.result_id);
		ctx.machine_result = result_index >= 0
			&& machine_value_used[
				static_cast<uint32_t>(result_index)] != 0;
	}
	if (ctx.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& ctx.instruction.direct_call != nullptr) {
		set_result_flag(ctx.instruction.direct_call->flags,
			ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT,
			ctx.machine_result
				&& zend_mir_scalar_type_is_exact(
					exact_type(ctx.result))
				&& exact_type(ctx.result)
					!= ZEND_MIR_SCALAR_TYPE_NULL);
	} else if (ctx.record.opcode
				== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
			&& ctx.instruction.direct_internal_call != nullptr) {
		set_result_flag(ctx.instruction.direct_internal_call->flags,
			ZEND_NATIVE_DIRECT_INTERNAL_CALL_REQUIRE_SCALAR_RESULT,
			ctx.machine_result);
	}
	ctx.inlined_user_body =
		function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& ctx.machine_result
			&& !zend_tpde_direct_call_has_argument_guard(ctx.instruction)
			&& frozen_effect_closed_inline(ctx.i)
			? inline_component_scalar_body(ctx.instruction,
				ctx.i,
				ctx.result,
				static_cast<uint32_t>(ctx.block),
				block_instructions)
			: InlinedBody{};
	/*
	 * Scalar identity operations without an exact machine result are
	 * registerless source-SSA topology without a target-side effect;
	 * exposing their operand to TPDE would create a use without a
	 * result definition.
	 */
	if (!ctx.machine_result
			&& (ctx.record.opcode == ZEND_MIR_OPCODE_COPY
				|| ctx.record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
				|| ctx.record.opcode == ZEND_MIR_OPCODE_I1_TO_I64)) {
		return false;
	}
	/*
	 * Constant-folded scalar results have no TPDE assignment: their
	 * consumers use the constant, and the pure defining instruction
	 * would only ask the allocator for an ignored value.
	 */
	if (!ctx.machine_result && !ctx.register_cond_branch
			&& ctx.record.opcode
				>= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
			&& ctx.record.opcode < ZEND_MIR_OPCODE_SCALAR_DROP) {
		return false;
	}
	if (ctx.machine_result
			&& ((ctx.record.opcode
						>= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
					&& ctx.record.opcode < ZEND_MIR_OPCODE_SCALAR_DROP
					&& !ctx.instruction.fused_into_branch)
				|| ((ctx.record.opcode == ZEND_MIR_OPCODE_COPY
						|| ctx.record.opcode
							== ZEND_MIR_OPCODE_CANONICALIZE
						|| ctx.record.opcode
							== ZEND_MIR_OPCODE_I1_TO_I64)
					&& ctx.record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL))) {
		entry_register_definitions[ctx.i] = 1;
	}
	return true;
}

/* The call descriptors are shared by the entry and the typed
 * body of a plan; the entry adaptor, built first, owns their
 * result contract. */
void ZendIRAdaptor::Builder::set_result_flag(
		uint32_t &flags, uint32_t flag, bool required) {
	if (function_mode_ == FunctionMode::TypedBody) {
		ZEND_ASSERT(((flags & flag) != 0) == required);
		return;
	}
	flags = required ? (flags | flag) : (flags & ~flag);
}

bool ZendIRAdaptor::Builder::lower_boxed_copy(InstContext &ctx) {
	/* Pi nodes over canonical zvals preserve source SSA topology.
	 * They are registerless only after the same physical-location proof
	 * used for boxed PHIs. */
	if (ctx.record.opcode == ZEND_MIR_OPCODE_COPY
			&& ctx.record.representation
				== ZEND_MIR_REPRESENTATION_ZVAL) {
		uint64_t copy_constant_bits;
		if (ctx.machine_result
				&& ctx.copy_input != INVALID_VALUE_REF
					&& ((machine_value_is_register_authoritative(
							ctx.copy_input)
						&& machine_value_has_register_definition(
							ctx.copy_input))
						|| entry_argument_has_register_definition(
							ctx.copy_input)
						|| entry_register_definition(ctx.copy_input)
						|| constant(ctx.copy_input, &copy_constant_bits))) {
			/* Emit the selected machine copy below. */
			entry_register_definitions[ctx.i] = 1;
		} else {
			const zend_mir_storage_id result_storage =
				canonical_storage(ctx.result);
			const zend_mir_value_id input_id =
				ctx.instruction.operand_count == 1
					? zend_tpde_operand_at(plan_, &ctx.instruction, 0)
					: ZEND_MIR_ID_INVALID;
			const int32_t input_index = zend_tpde_value_index(
				plan_, input_id);
			const IRValueRef input = input_index >= 0
				? value_ref(input_id)
				: INVALID_VALUE_REF;
			if (ctx.result == INVALID_VALUE_REF
					|| input_index < 0
					|| input == INVALID_VALUE_REF
					|| plan_->values[static_cast<uint32_t>(input_index)]
						.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL
					|| !zend_mir_id_is_valid(result_storage)
					|| canonical_storage(input) != result_storage) {
				valid_ = false;
			}
			if (ctx.machine_result
					&& ctx.result != INVALID_VALUE_REF
					&& input != INVALID_VALUE_REF
					&& zend_tpde_machine_kind_is_pointer(
						machine_kind(ctx.result))
					&& machine_kind(input)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& representation(input)
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& zend_mir_id_is_valid(result_storage)
					&& canonical_storage(input) == result_storage) {
				const IRValueRef address = frame_slot_address(result_storage);
				if (address == INVALID_VALUE_REF) {
					valid_ = false;
					return false;
				}
				const uint32_t load_operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(address);
				add_node(block_instructions, ctx.block, InstNode{
					.kind = InstKind::ZvalPayloadLoad,
					.mir_instruction_index = ctx.i,
					.argument_index = UINT32_MAX,
					.result = ctx.result,
					.operand_offset = load_operand_offset,
					.operand_count = 1,
					.has_result = true,
					.storage_id = result_storage,
					.exact_type = exact_type(ctx.result)});
				set_instruction_result(ctx.i, ctx.result);
			}
			return false;
		}
	}
	return true;
}

void ZendIRAdaptor::Builder::add_value_assign_operands(InstContext &ctx) {
	if ((ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
				|| ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN)
			&& ctx.instruction.has_value_operation) {
		IRValueRef assigned = ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN
			? source_binding_value_ref(
				ctx.instruction.source_op1_binding)
			: ctx.i < register_assignment_sources.size()
					&& register_assignment_sources[ctx.i]
						!= INVALID_VALUE_REF
				? register_assignment_sources[ctx.i]
				: source_binding_value_ref(
					ctx.instruction.source_op2_binding);
		if (assigned == INVALID_VALUE_REF
				&& ctx.record.opcode != ZEND_MIR_OPCODE_VALUE_QM_ASSIGN) {
			assigned = source_binding_value_ref(
				ctx.instruction.source_op1_binding);
		}
		if (assigned != INVALID_VALUE_REF
				&& machine_value_is_register_authoritative(
					assigned)
				&& machine_value_has_register_definition(
					assigned)) {
			/*
			 * A register-produced temporary stays authoritative through
			 * assignment: the target writes the destination from its
			 * scalar, pointer or boxed parts, and later source SSA
			 * users consume the same value instead of reloading the CV.
			 */
			operands_.push_back(assigned);
			const uint32_t definition_plus_one =
				ctx.instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one;
				if (definition_plus_one != 0) {
					const uint32_t definition =
						definition_plus_one - 1;
					const IRValueRef assignment_result =
						ctx.i < register_assignment_results.size()
								&& register_assignment_results[ctx.i]
									!= INVALID_VALUE_REF
							? register_assignment_results[ctx.i]
							: assigned;
					auto &source_overrides =
						active_source_ssa_overrides();
					if (definition >= source_overrides.size()) {
						valid_ = false;
					} else {
						source_overrides[definition] =
							assignment_result;
					const int32_t value_index =
						zend_tpde_value_index(
							plan_,
							zend_mir_value_from_original_ssa(
								definition));
					if (value_index >= 0
							&& static_cast<uint32_t>(value_index)
								< active_value_overrides().size()) {
							active_value_overrides()[
								static_cast<uint32_t>(value_index)] =
									assignment_result;
					}
				}
			}
		}
	}
}

void ZendIRAdaptor::Builder::add_fetch_operands(InstContext &ctx) {
	if ((ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				|| ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM)
			&& ctx.instruction.has_value_operation) {
		IRValueRef key = source_binding_value_ref(
			ctx.instruction.source_op2_binding);
		if (key == INVALID_VALUE_REF) {
			key = source_operand_value_ref(
				ctx.instruction.value_operation.op2);
		}
		if (key != INVALID_VALUE_REF
				&& ((exact_type(key) == ZEND_MIR_SCALAR_TYPE_I64
						&& representation(key)
							== ZEND_MIR_REPRESENTATION_I64
						&& machine_kind(key)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (machine_kind(key)
							== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						&& machine_value_is_register_authoritative(key))
					|| (representation(key)
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& machine_kind(key)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
				&& machine_value_has_register_definition(key)) {
			/*
			 * A proven integer or string key stays in TPDE SSA: the
			 * target selects a packed lookup or a pointer-identity
			 * mixed lookup without reloading the key zval.
			 */
			operands_.push_back(key);
			const IRValueRef receiver = source_binding_value_ref(
				ctx.instruction.source_op1_binding);
			if (receiver != INVALID_VALUE_REF
					&& (machine_kind(receiver)
							== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
						|| (representation(receiver)
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& machine_kind(receiver)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
					&& machine_value_has_register_definition(receiver)) {
				/*
				 * Array-producing operations publish their zval for PHP
				 * observability and keep the payload pointer as the
				 * TPDE value. It follows the key, so layouts that only
				 * read the key are unchanged.
				 */
				operands_.push_back(receiver);
			}
		}
	}
	if (ctx.record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			&& ctx.instruction.has_value_operation) {
		IRValueRef name = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if (name == INVALID_VALUE_REF) {
			name = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		if (name != INVALID_VALUE_REF
				&& machine_kind(name)
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				&& machine_value_is_register_authoritative(name)
				&& machine_value_has_register_definition(name)) {
			/*
			 * A dynamic CV lookup consumes the variable name on every
			 * execution: the guarded fast node takes the zend_string
			 * pointer; the cold helper reads the materialized frame.
			 */
			operands_.push_back(name);
		}
	}
}

void ZendIRAdaptor::Builder::add_compound_assign_operands(InstContext &ctx) {
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& ctx.instruction.has_value_operation) {
			const IRValueRef right = source_binding_value_ref(
				ctx.instruction.source_op2_binding);
			const IRValueRef left = source_binding_value_ref(
				ctx.instruction.source_op1_binding);
			const bool register_right =
				plan_->generator_resume_count == 0
				&& !storage_assigned_by_reference(
					ctx.instruction.value_operation.op2_storage_id)
				&& right != INVALID_VALUE_REF
				&& ((representation(right)
							== ZEND_MIR_REPRESENTATION_I64
						&& exact_type(right)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(right)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| machine_kind(right)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
				&& machine_value_is_register_authoritative(right)
				&& machine_value_has_register_definition(right);
			const bool register_left =
				plan_->generator_resume_count == 0
				&& !storage_assigned_by_reference(
					ctx.instruction.value_operation.op1_storage_id)
				&& left != INVALID_VALUE_REF
				&& ((representation(left)
							== ZEND_MIR_REPRESENTATION_I64
						&& exact_type(left)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(left)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| machine_kind(left)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
				&& machine_value_is_register_authoritative(left)
				&& machine_value_has_register_definition(left);
			if (register_right) {
				/*
				 * A preceding register result survives a compound
				 * assignment: the target consumes the payload/type pair
				 * directly, and only the cold helper materializes it.
				 */
				ctx.assign_op_right_operand_index =
					static_cast<uint32_t>(operands_.size())
						- ctx.operand_offset;
				operands_.push_back(right);
			}
			if (register_left) {
				ctx.assign_op_left_operand_index =
					static_cast<uint32_t>(operands_.size())
						- ctx.operand_offset;
				operands_.push_back(left);
			}
		}
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_INCDEC
			&& ctx.instruction.has_value_operation
			&& ctx.instruction.mutation_lazy_scalar) {
		const IRValueRef operand = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if (operand != INVALID_VALUE_REF
				&& machine_value_is_register_authoritative(operand)
				&& machine_value_has_register_definition(operand)
				&& ((representation(operand)
							== ZEND_MIR_REPRESENTATION_I64
						&& exact_type(operand)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| machine_kind(operand)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
			operands_.push_back(operand);
		}
	}
}

void ZendIRAdaptor::Builder::add_write_value_operands(InstContext &ctx) {
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
			&& ctx.instruction.has_value_operation) {
		zend_tpde_packed_array_append append_layout{};
		IRValueRef value = source_binding_value_ref(
			ctx.instruction.source_auxiliary_binding);
		if (value == INVALID_VALUE_REF) {
			value = source_operand_value_ref(
				ctx.instruction.value_operation.auxiliary);
		}
		if (zend_tpde_packed_array_append_at(
				ctx.instruction, &append_layout)
				&& value != INVALID_VALUE_REF
				&& representation(value)
					== ZEND_MIR_REPRESENTATION_I64
				&& exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(value)
					== ZEND_TPDE_MACHINE_VALUE_I64
				&& machine_value_is_register_authoritative(value)
				&& machine_value_has_register_definition(value)) {
			/*
			 * The append helper records the auxiliary source as a
			 * semantic input. The guarded fast node also takes its
			 * machine payload, so it need not reload, classify and
			 * refcount the zval; the cold node skips this use and reads
			 * the frame.
			 */
			ctx.packed_append_value_operand_index =
				static_cast<uint32_t>(operands_.size())
					- ctx.operand_offset;
			operands_.push_back(value);
		}
	}
	if (ctx.record.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN
			&& ctx.instruction.has_value_operation) {
		zend_tpde_object_property_write property_layout{};
		IRValueRef value = source_binding_value_ref(
			ctx.instruction.source_auxiliary_binding);
		if (value == INVALID_VALUE_REF) {
			value = source_operand_value_ref(
				ctx.instruction.value_operation.auxiliary);
		}
		if (zend_tpde_object_property_write_at(
				ctx.instruction, &property_layout)
				&& value != INVALID_VALUE_REF
				&& representation(value)
					== ZEND_MIR_REPRESENTATION_I64
				&& exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(value)
					== ZEND_TPDE_MACHINE_VALUE_I64
				&& machine_value_is_register_authoritative(value)
				&& machine_value_has_register_definition(value)) {
			ctx.property_write_value_operand_index =
				static_cast<uint32_t>(operands_.size())
					- ctx.operand_offset;
			operands_.push_back(value);
		}
	}
}

void ZendIRAdaptor::Builder::add_register_unary_operands(InstContext &ctx) {
	if (ctx.type_check_selection
			!= ScalarTypeCheckSelection::Invalid) {
		switch (ctx.type_check_selection) {
			case ScalarTypeCheckSelection::CopyInput:
			case ScalarTypeCheckSelection::NotInput:
				operands_.push_back(ctx.type_check_input);
				break;
			case ScalarTypeCheckSelection::ConstantFalse:
			case ScalarTypeCheckSelection::ConstantTrue:
				operands_.push_back(add_derived_value(
					ZEND_MIR_REPRESENTATION_I1,
					ZEND_MIR_SCALAR_TYPE_I1,
					ZEND_MIR_ID_INVALID, true,
					ctx.type_check_selection
							== ScalarTypeCheckSelection::ConstantTrue
						? 1 : 0));
				break;
			default:
				break;
		}
	}
	if (ctx.register_cond_branch) {
		operands_.push_back(ctx.register_condition);
	}
	if (ctx.register_bool_unary_operand != INVALID_VALUE_REF) {
		/*
		 * BOOL and BOOL_NOT of a proven machine boolean skip the
		 * temporary: the fast node takes the source value, the cold
		 * helper reads the frame.
		 */
		operands_.push_back(ctx.register_bool_unary_operand);
	}
	if (ctx.register_string_length_operand != INVALID_VALUE_REF) {
		/*
		 * The zend_string pointer precedes the frame operand: the fast
		 * node reads the length directly; a generic cold node skips
		 * this use and reads the source zval.
		 */
		operands_.push_back(ctx.register_string_length_operand);
	}
}

void ZendIRAdaptor::Builder::add_slot_isset_operand(InstContext &ctx) {
	ctx.static_slot_isset_operand = INVALID_VALUE_REF;
	bool static_slot_isset_exact = false;
	ctx.static_slot_isset_needs_value = false;
	ctx.static_slot_isset_exact_type =
		ZEND_MIR_SCALAR_TYPE_NONE;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
			&& ctx.instruction.has_value_operation) {
		zend_tpde_slot_isset_empty isset_layout{};
		const bool has_isset_layout =
			zend_tpde_slot_isset_empty_at(ctx.instruction, &isset_layout);
		IRValueRef candidate = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if ((candidate == INVALID_VALUE_REF
				|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE)
				&& ctx.instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one != 0) {
			candidate = value_ref(zend_mir_value_from_original_ssa(
				ctx.instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one - 1));
		}
		if (candidate == INVALID_VALUE_REF
				|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE) {
			candidate = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		uint64_t constant_bits = 0;
		const zend_mir_scalar_type_mask type =
			candidate == INVALID_VALUE_REF
				? ZEND_MIR_SCALAR_TYPE_NONE
				: exact_type(candidate);
		const bool supported =
			type == ZEND_MIR_SCALAR_TYPE_NULL
			|| type == ZEND_MIR_SCALAR_TYPE_I1
			|| type == ZEND_MIR_SCALAR_TYPE_I64;
		static_slot_isset_exact = supported && has_isset_layout
			&& machine_value_has_stable_exact_type(candidate);
		ctx.static_slot_isset_needs_value = static_slot_isset_exact
			&& ctx.machine_result
			&& isset_layout.is_empty
			&& type != ZEND_MIR_SCALAR_TYPE_NULL;
		bool available = candidate != INVALID_VALUE_REF
			&& (constant(candidate, &constant_bits)
				|| (machine_value_is_register_authoritative(candidate)
					&& machine_value_has_register_definition(candidate)));
		if (ctx.static_slot_isset_needs_value && !available
				&& type != ZEND_MIR_SCALAR_TYPE_NULL
				&& function_mode_ == FunctionMode::ZendEntry) {
			const zend_mir_storage_id storage_id =
				canonical_storage(candidate);
			const IRValueRef address = frame_slot_address(storage_id);
			const IRValueRef transported = address != INVALID_VALUE_REF
				? add_derived_value(
					representation(candidate), type, storage_id, false, 0,
					machine_kind(candidate),
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					refcount_state(candidate))
				: INVALID_VALUE_REF;
			if (transported != INVALID_VALUE_REF) {
				add_payload_load_before(block_instructions,
					guarded_hot_blocks[ctx.i] != UINT32_MAX
						? guarded_hot_blocks[ctx.i]
						: static_cast<uint32_t>(ctx.block),
					ctx.i, address, transported, storage_id, type,
					ctx.operand_offset);
				candidate = transported;
				available = true;
			}
		}
		if (static_slot_isset_exact) {
			ctx.static_slot_isset_exact_type = type;
			if (ctx.static_slot_isset_needs_value && available) {
				ctx.static_slot_isset_operand = candidate;
				operands_.push_back(candidate);
			}
		}
	}
	ctx.static_slot_isset_machine_fast =
		static_slot_isset_exact
		&& (!ctx.static_slot_isset_needs_value
			|| ctx.static_slot_isset_operand != INVALID_VALUE_REF);
}

void ZendIRAdaptor::Builder::add_long_binary_operands(InstContext &ctx) {
	IRValueRef long_left = INVALID_VALUE_REF;
	IRValueRef long_right = INVALID_VALUE_REF;
	ctx.explicit_long_operands =
		ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
		&& ctx.machine_result
		&& long_binary_machine_operands(
			ctx.instruction, long_left, long_right);
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
			&& ctx.machine_result && !ctx.explicit_long_operands) {
		long_left = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if (long_left == INVALID_VALUE_REF) {
			long_left = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		long_right = source_binding_value_ref(
			ctx.instruction.source_op2_binding);
		if (long_right == INVALID_VALUE_REF) {
			long_right = source_operand_value_ref(
				ctx.instruction.value_operation.op2);
		}
		const uint32_t source_opcode =
			ctx.instruction.value_operation.source_opcode;
		const bool supported_opcode =
			long_binary_source_opcode(source_opcode);
		const bool left_supported = long_left != INVALID_VALUE_REF
			&& (exact_type(long_left) == ZEND_MIR_SCALAR_TYPE_I64
				|| machine_kind(long_left)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		const bool right_supported = long_right != INVALID_VALUE_REF
			&& (exact_type(long_right) == ZEND_MIR_SCALAR_TYPE_I64
				|| machine_kind(long_right)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		ctx.explicit_long_operands = supported_opcode
			&& left_supported && right_supported
			&& transport_canonical_long(ctx, long_left)
			&& transport_canonical_long(ctx, long_right)
			&& machine_value_has_register_definition(long_left)
			&& machine_value_has_register_definition(long_right);
	}
	if (ctx.explicit_long_operands) {
		operands_.push_back(long_left);
		operands_.push_back(long_right);
	}
}

bool ZendIRAdaptor::Builder::transport_canonical_long(
		InstContext &ctx, IRValueRef &value) {
	if (value == INVALID_VALUE_REF
			|| machine_value_has_register_definition(value)) {
		return value != INVALID_VALUE_REF;
	}
	if (exact_type(value) != ZEND_MIR_SCALAR_TYPE_I64
			|| machine_kind(value)
				!= ZEND_TPDE_MACHINE_VALUE_I64) {
		return false;
	}
	const zend_mir_storage_id storage_id =
		canonical_storage(value);
	const IRValueRef address = frame_slot_address(storage_id);
	const IRValueRef transported = address != INVALID_VALUE_REF
		? add_derived_value(
			ZEND_MIR_REPRESENTATION_I64,
			ZEND_MIR_SCALAR_TYPE_I64, storage_id,
			false, 0, ZEND_TPDE_MACHINE_VALUE_I64,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_IMMORTAL)
		: INVALID_VALUE_REF;
	if (transported == INVALID_VALUE_REF) {
		return false;
	}
	add_payload_load_before(block_instructions, ctx.block, ctx.i,
		address, transported, storage_id,
		ZEND_MIR_SCALAR_TYPE_I64, ctx.operand_offset);
	value = transported;
	return true;
}

void ZendIRAdaptor::Builder::add_literal_reference_operand(InstContext &ctx) {
	ctx.machine_reference_operand_index = UINT32_MAX;
	ctx.direct_call_context_operand = UINT32_MAX;
	ctx.direct_internal_argument_transport = false;
	ctx.direct_internal_boxed_argument_count = 0;
	const bool literal_assign_operand =
		ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
		&& ctx.instruction.source_op2_reference_index
				< plan_->machine_reference_count
			&& plan_->machine_references[
				ctx.instruction.source_op2_reference_index].kind
				== ZEND_TPDE_MACHINE_REFERENCE_LITERAL;
	const bool literal_array_receiver =
		ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
		&& ctx.instruction.value_operation.op1.kind
			== ZEND_MIR_SOURCE_OPERAND_LITERAL
		&& ctx.instruction.source_op1_reference_index
			< plan_->machine_reference_count
		&& plan_->machine_references[
			ctx.instruction.source_op1_reference_index].kind
			== ZEND_TPDE_MACHINE_REFERENCE_LITERAL;
	if (literal_assign_operand || literal_array_receiver) {
		const uint32_t reference_index = literal_array_receiver
			? ctx.instruction.source_op1_reference_index
			: ctx.instruction.source_op2_reference_index;
		const IRValueRef literal_address = add_derived_value(
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_MIR_ID_INVALID, false, 0, UINT8_MAX,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN,
			reference_index);
		ctx.machine_reference_operand_index =
			static_cast<uint32_t>(operands_.size())
				- ctx.operand_offset;
		operands_.push_back(literal_address);
	}
}

void ZendIRAdaptor::Builder::add_data_operands(InstContext &ctx) {
	/*
	 * A lazy store leaves its CV slot to the boundaries, which store
	 * the source's register value.  A source without one here, such
	 * as a frame-published call result, is stored at once.
	 */
	ctx.lazy_store = false;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
			&& ctx.instruction.zval_store_lazy_scalar
			&& ctx.instruction.operand_count >= 1) {
		/* Consumers read the source itself, so it must be the
		 * register value a boundary stores. */
		const int32_t source_index = zend_tpde_value_index(plan_,
			zend_tpde_operand_at(plan_, &ctx.instruction, 0));
		const IRValueRef source = source_index < 0
			? INVALID_VALUE_REF
			: resolve_materializable_scalar(
				static_cast<uint32_t>(source_index),
				static_cast<uint32_t>(ctx.block), ctx.i, 0);
		ctx.lazy_store = source != INVALID_VALUE_REF
			&& source == value_ref(zend_tpde_operand_at(
				plan_, &ctx.instruction, 0))
			&& machine_value_has_result_representation(source)
			&& machine_value_has_register_definition(source);
	}
	/*
	 * RETURN_SOURCE_ZVAL transfers the canonical zval from the frame,
	 * selected by its source opline: its MIR operand only carries
	 * dependency and type facts, and a machine value for it would stay
	 * unconsumed and could be mistaken for a complete zval.
	 */
	ctx.data_operand_count =
		ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
			? ctx.lazy_store ? 0 : 1
		: ctx.static_slot_isset_operand != INVALID_VALUE_REF
			? 0
		: ctx.register_bool_unary_operand != INVALID_VALUE_REF
			? 0
		: ctx.explicit_long_operands
			? 0
		: ctx.type_check_selection
					!= ScalarTypeCheckSelection::Invalid
				|| ctx.register_cond_branch
			? 0
		: ctx.boxed_cond_branch
			? 0
		: (ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& function_mode_ == FunctionMode::ZendEntry)
			? 0
		: ctx.record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| (ctx.record.opcode
					== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& function_mode_
					== FunctionMode::ZendEntry)
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
			|| (ctx.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& ctx.instruction.direct_call != nullptr)
			? 0 : ctx.instruction.operand_count;
	append_data_operands(ctx);
}

void ZendIRAdaptor::Builder::add_frame_and_call_operands(InstContext &ctx) {
	if (ctx.record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
		/*
		 * A proven return-type check emits no instruction but stays an
		 * SSA use, so a PHI feeding the proof is released at the check
		 * instead of looking live at the block boundary.
		 */
		IRValueRef verified = source_binding_value_ref(
			ctx.instruction.source_op1_binding);
		if (verified == INVALID_VALUE_REF) {
			verified = source_operand_value_ref(
				ctx.instruction.value_operation.op1);
		}
		if (verified != INVALID_VALUE_REF
				&& (machine_value_has_register_definition(
						verified)
					|| val_is_phi(verified))) {
			operands_.push_back(verified);
		}
	}
	if (ctx.record.opcode
			== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
		add_return_source_operands(ctx.i, ctx.instruction);
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& (ctx.record.opcode == ZEND_MIR_OPCODE_RETURN
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
			|| ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE)
			&& !(ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				&& ctx.static_slot_isset_machine_fast
				&& ctx.machine_result
				&& guarded_cold_blocks[ctx.i] == UINT32_MAX)) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			&& (ctx.record.effects & ZEND_MIR_EFFECT_MASK(
				ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
		operands_.push_back(
			IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& zend_mir_opcode_is_executable_value(ctx.record.opcode)
			&& !ctx.boxed_cond_branch
			&& !ctx.register_cond_branch
			&& (ctx.register_bool_unary_operand == INVALID_VALUE_REF
				|| guarded_cold_blocks[ctx.i] != UINT32_MAX)
			&& !(ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				&& ctx.static_slot_isset_machine_fast
				&& ctx.machine_result
				&& guarded_cold_blocks[ctx.i] == UINT32_MAX)
			&& !(ctx.record.opcode
					== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
				&& ctx.instruction.runtime_helper
					== ZEND_NATIVE_HELPER_COUNT)) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode
			== ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.boxed_cond_branch) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
		if (boxed_cond_cold_blocks[ctx.i] != UINT32_MAX) {
			if (ctx.register_boxed_condition_operand != INVALID_VALUE_REF) {
				operands_.push_back(ctx.register_boxed_condition_operand);
			} else if (ctx.register_string_condition_operand
					!= INVALID_VALUE_REF) {
				operands_.push_back(ctx.register_string_condition_operand);
			}
		} else if (ctx.register_boxed_condition_operand
					!= INVALID_VALUE_REF) {
			operands_.push_back(ctx.register_boxed_condition_operand);
		}
	}
	if (ctx.record.opcode
			== ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		add_user_call_operands(ctx);
	} else if (ctx.record.opcode
			== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
		add_internal_call_operands(ctx);
	} else if (ctx.record.opcode
			== ZEND_MIR_OPCODE_CATCH_ENTER
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_FINALLY_ENTER
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_FINALLY_CALL) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	} else if (ctx.record.opcode
			== ZEND_MIR_OPCODE_FINALLY_RETURN) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
}

void ZendIRAdaptor::Builder::count_operands(InstContext &ctx) {
	ctx.operand_count =
		static_cast<uint32_t>(operands_.size()) - ctx.operand_offset;
	ctx.inlined_operand_index =
		ctx.inlined_user_body.valid
			? ctx.operand_count - ctx.inlined_user_body.operand_count()
			: UINT32_MAX;
	ctx.materialization_count =
		function_mode_ == FunctionMode::ZendEntry
			? ctx.instruction.materialization_count : 0;
	ctx.materialization_operand_index =
		ctx.materialization_count == 0
			? UINT32_MAX : ctx.operand_count;
	add_materialization_operands(ctx);
	ctx.boxed_cond_cold_block =
		boxed_cond_cold_blocks[ctx.i];
	ctx.guarded_cold_block =
		guarded_cold_blocks[ctx.i];
}

void ZendIRAdaptor::Builder::add_boxed_boundary_operands(InstContext &ctx) {
	ctx.boxed_op1_boundary = INVALID_VALUE_REF;
	ctx.boxed_op2_boundary = INVALID_VALUE_REF;
	ctx.boxed_op1_boundary_operand_index = UINT32_MAX;
	ctx.boxed_op2_boundary_operand_index = UINT32_MAX;
	ctx.boundary_semantic_operand_count = UINT32_MAX;
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.instruction.has_value_operation
			&& zend_mir_opcode_is_executable_value(ctx.record.opcode)) {
		ctx.boxed_op1_boundary = direct_boxed_source(ctx,
			ctx.instruction.source_op1_binding,
			ctx.instruction.value_operation.op1,
			ctx.instruction.value_operation.op1_storage_id,
			ctx.instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one);
		ctx.boxed_op2_boundary = direct_boxed_source(ctx,
			ctx.instruction.source_op2_binding,
			ctx.instruction.value_operation.op2,
			ctx.instruction.value_operation.op2_storage_id,
			ctx.instruction
				.source_op2_definition_ssa_variable_id_plus_one);
	}
	if (ctx.boxed_cond_cold_block == UINT32_MAX
			&& ctx.guarded_cold_block == UINT32_MAX) {
		const uint32_t semantic_count =
			ctx.materialization_operand_index == UINT32_MAX
				? ctx.operand_count : ctx.materialization_operand_index;
		ctx.boxed_op1_boundary_operand_index = append_boundary_operand(
			ctx.boxed_op1_boundary, ctx.operand_offset, semantic_count,
			ctx.operand_count);
		if (ctx.boxed_op2_boundary
				== (ctx.boxed_op1_boundary_operand_index == UINT32_MAX
				? INVALID_VALUE_REF
				: operands_[ctx.operand_offset
					+ ctx.boxed_op1_boundary_operand_index])) {
			ctx.boxed_op2_boundary_operand_index =
				ctx.boxed_op1_boundary_operand_index;
		} else {
			ctx.boxed_op2_boundary_operand_index =
				append_boundary_operand(ctx.boxed_op2_boundary,
					ctx.operand_offset, semantic_count, ctx.operand_count);
		}
		if (ctx.boxed_op1_boundary_operand_index != UINT32_MAX
				|| ctx.boxed_op2_boundary_operand_index != UINT32_MAX) {
			ctx.boundary_semantic_operand_count = semantic_count;
		}
	}
}

IRValueRef ZendIRAdaptor::Builder::direct_boxed_source(InstContext &ctx,
		const zend_tpde_source_value_binding &binding,
		const zend_mir_source_operand_ref &source,
		zend_mir_storage_id storage,
		uint32_t definition_ssa_variable_id_plus_one) {
	IRValueRef candidate = source_binding_value_ref(binding);
	if ((candidate == INVALID_VALUE_REF
			|| machine_kind(candidate)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
			&& definition_ssa_variable_id_plus_one != 0) {
		candidate = value_ref(zend_mir_value_from_original_ssa(
			definition_ssa_variable_id_plus_one - 1));
	}
	if (candidate == INVALID_VALUE_REF) {
		candidate = source_operand_value_ref(source);
	}
	int32_t definition = binding.definition_instruction_index;
	if (definition < 0 && binding.value_index >= 0
			&& static_cast<uint32_t>(binding.value_index)
				< source_producer_by_value.size()) {
		definition = source_producer_by_value[
			static_cast<uint32_t>(binding.value_index)];
	}
	if (definition < 0 && candidate != INVALID_VALUE_REF) {
		const uint32_t candidate_index =
			static_cast<uint32_t>(candidate);
		definition = candidate_index
				< latest_source_producer_by_ir_value.size()
			? latest_source_producer_by_ir_value[candidate_index] : -1;
	}
	const bool direct_definition = definition >= 0
		&& static_cast<uint32_t>(definition)
			< active_instruction_results().size()
		&& static_cast<uint32_t>(definition)
			< plan_->instruction_count
		&& (plan_->instructions[
			static_cast<uint32_t>(definition)].record.opcode
				== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			|| plan_->instructions[
				static_cast<uint32_t>(definition)].record.opcode
				== ZEND_MIR_OPCODE_OBJECT_FETCH_R)
		&& static_cast<uint32_t>(definition)
			< source_result_consumer.size()
		&& source_result_consumer[
			static_cast<uint32_t>(definition)]
			== static_cast<int32_t>(ctx.i);
	const IRValueRef produced = direct_definition
		? active_instruction_results()[
			static_cast<uint32_t>(definition)]
		: INVALID_VALUE_REF;
	return direct_definition
			&& produced != INVALID_VALUE_REF
			&& canonical_storage(produced) == storage
			&& machine_kind(produced)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& machine_value_is_register_authoritative(produced)
			&& machine_value_has_register_definition(produced)
		? produced : INVALID_VALUE_REF;
}

void ZendIRAdaptor::Builder::emit_instruction_node(InstContext &ctx) {
	add_node(block_instructions, static_cast<uint32_t>(ctx.block), InstNode{
		.kind = ctx.type_check_selection
					!= ScalarTypeCheckSelection::Invalid
				|| ctx.register_cond_branch
				|| ctx.register_bool_unary_operand != INVALID_VALUE_REF
			? InstKind::MIR
			: ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& !ctx.lazy_store
			? InstKind::ZvalStore
			: executable_kind(ctx.instruction, ctx.record),
		.mir_instruction_index = ctx.i,
		.argument_index = UINT32_MAX,
		.result = ctx.result,
		.operand_offset = ctx.operand_offset,
		.operand_count = ctx.operand_count,
		.has_result = ctx.machine_result,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ctx.static_slot_isset_machine_fast
			? ctx.static_slot_isset_exact_type
			: ctx.canonical_bool_unary_exact_type,
		.inlined_user_body = ctx.inlined_user_body.valid,
		.inlined_operand_index = ctx.inlined_operand_index,
		.materialization_operand_index = ctx.materialization_operand_index,
		.materialization_count = ctx.materialization_count});
	ctx.main_node = static_cast<uint32_t>(nodes_.size() - 1);
	nodes_.back().direct_internal_argument_transport =
		ctx.direct_internal_argument_transport;
	nodes_.back().assign_op_right_operand_index =
		ctx.assign_op_right_operand_index;
	nodes_.back().assign_op_left_operand_index =
		ctx.assign_op_left_operand_index;
	nodes_.back().packed_append_value_operand_index =
		ctx.packed_append_value_operand_index;
	nodes_.back().property_write_value_operand_index =
		ctx.property_write_value_operand_index;
	nodes_.back().semantic_operand_count =
		ctx.boundary_semantic_operand_count;
	nodes_.back().boxed_op1_boundary_operand_index =
		ctx.boxed_op1_boundary_operand_index;
	nodes_.back().boxed_op2_boundary_operand_index =
		ctx.boxed_op2_boundary_operand_index;
	if (!ctx.inlined_user_body.checked_steps.empty()) {
		nodes_.back().inlined_checked_step_offset =
			static_cast<uint32_t>(inlined_checked_steps_.size());
		nodes_.back().inlined_checked_step_count =
			static_cast<uint32_t>(
				ctx.inlined_user_body.checked_steps.size());
		nodes_.back().inlined_checked_operand_count =
			ctx.inlined_user_body.operand_count();
		inlined_checked_steps_.insert(
			inlined_checked_steps_.end(),
			ctx.inlined_user_body.checked_steps.begin(),
			ctx.inlined_user_body.checked_steps.end());
	}
	nodes_.back().mutation_result =
		ctx.i < mutation_results.size()
			&& mutation_results[ctx.i] != INVALID_VALUE_REF;
	materialize_boxed_result_for_helper(
		ctx.i, static_cast<uint32_t>(ctx.block), ctx.result);
	reload_slot_authoritative_source_result(
		ctx.i, ctx.instruction, ctx.record, static_cast<uint32_t>(ctx.block));
}

void ZendIRAdaptor::Builder::publish_init_array_pointer(InstContext &ctx) {
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_INIT_ARRAY
			&& ctx.instruction.has_value_operation) {
		auto &value_overrides = active_value_overrides();
		auto &source_overrides =
			active_source_ssa_overrides();
		const IRValueRef canonical_result =
			source_binding_value_ref(
				ctx.instruction.source_result_binding);
		const uint32_t canonical_index =
			static_cast<uint32_t>(canonical_result);
		const zend_mir_storage_id storage_id =
			ctx.instruction.value_operation.result_storage_id;
		const IRValueRef address =
			frame_slot_address(storage_id);
		const IRValueRef array =
			address != INVALID_VALUE_REF
				? add_derived_value(
					ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
					ZEND_MIR_SCALAR_TYPE_NONE,
					storage_id, false, 0,
					ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN)
				: INVALID_VALUE_REF;
		if (canonical_result == INVALID_VALUE_REF
				|| !is_mir_value(canonical_index)
				|| array == INVALID_VALUE_REF) {
			valid_ = false;
		} else {
			const uint32_t load_operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(address);
			add_node(block_instructions,
				static_cast<uint32_t>(ctx.block), InstNode{
					.kind = InstKind::ZvalPayloadLoad,
					.mir_instruction_index = ctx.i,
					.argument_index = UINT32_MAX,
					.result = array,
					.operand_offset = load_operand_offset,
					.operand_count = 1,
					.has_result = true,
					.storage_id = storage_id,
					.exact_type = ZEND_MIR_SCALAR_TYPE_NONE});
			set_instruction_result(ctx.i, array);
			value_overrides[
				canonical_index - MIR_VALUE_BASE] = array;
			const uint32_t result_ssa =
				ctx.instruction.value_operation.result
					.ssa_variable_id;
			if (result_ssa < source_overrides.size()) {
				source_overrides[result_ssa] = array;
			}
		}
	}
}

void ZendIRAdaptor::Builder::mark_selected_operation(InstContext &ctx) {
	if (ctx.type_check_selection
			!= ScalarTypeCheckSelection::Invalid
			|| ctx.register_cond_branch
			|| ctx.register_bool_unary_operand != INVALID_VALUE_REF) {
		InstNode &selected = nodes_[ctx.main_node];
		selected.synthetic = true;
		selected.synthetic_record = ctx.record;
		selected.synthetic_record.opcode =
			ctx.register_cond_branch
				? ZEND_MIR_OPCODE_COND_BRANCH
			: ctx.register_bool_unary_operand != INVALID_VALUE_REF
				? ctx.instruction.value_operation.source_opcode
						== ZEND_BOOL_NOT
					? ZEND_MIR_OPCODE_I1_NOT
					: ZEND_MIR_OPCODE_COPY
			: ctx.type_check_selection
					== ScalarTypeCheckSelection::NotInput
				? ZEND_MIR_OPCODE_I1_NOT
				: ZEND_MIR_OPCODE_COPY;
		selected.synthetic_record.effects = 0;
		selected.synthetic_record.reads = 0;
		selected.synthetic_record.writes = 0;
		selected.synthetic_record.barriers = 0;
		selected.synthetic_record.ownership_actions = 0;
		if (!ctx.register_cond_branch) {
			selected.synthetic_record.representation =
				ZEND_MIR_REPRESENTATION_I1;
			selected.synthetic_record.result_id =
				ZEND_MIR_ID_INVALID;
		}
	}
}

void ZendIRAdaptor::Builder::lower_typed_body_operation(
		uint32_t i, const zend_tpde_instruction &instruction, uint32_t block) {
	auto operand = [&](const zend_tpde_source_value_binding &binding,
			const zend_mir_source_operand_ref &source) {
		IRValueRef value = source_binding_value_ref(binding);
		if (value == INVALID_VALUE_REF) {
			value = source_operand_value_ref(source);
		}
		/* A pi COPY carries its input unchanged. */
		for (uint32_t depth = 0; value != INVALID_VALUE_REF
				&& depth < plan_->value_count; ++depth) {
			const uint32_t raw = static_cast<uint32_t>(value);
			if (!is_mir_value(raw)) {
				break;
			}
			const int32_t definition =
				plan_->value_definition_instructions[
					raw - MIR_VALUE_BASE];
			if (definition < 0
					|| instruction_record_at(
						static_cast<uint32_t>(definition)).opcode
						!= ZEND_MIR_OPCODE_COPY
					|| plan_->instructions[definition]
						.operand_count != 1) {
				break;
			}
			const IRValueRef input = value_ref(zend_tpde_operand_at(
				plan_, &plan_->instructions[definition], 0));
			if (input == INVALID_VALUE_REF
					|| representation(input)
						!= representation(value)
					|| machine_kind(input) != machine_kind(value)) {
				break;
			}
			value = input;
		}
		return value;
	};
	const IRValueRef left = operand(
		instruction.source_op1_binding,
		instruction.value_operation.op1);
	const IRValueRef right = operand(
		instruction.source_op2_binding,
		instruction.value_operation.op2);
	const zend_tpde_machine_value_kind result_kind =
		zend_tpde_typed_numeric_result_kind(plan_, instruction);
	const IRValueRef result =
		result_kind == ZEND_TPDE_MACHINE_VALUE_I64
		? add_derived_value(
			ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_SCALAR_TYPE_I64,
			ZEND_MIR_ID_INVALID, false, 0,
			ZEND_TPDE_MACHINE_VALUE_I64,
			ZEND_MIR_OWNERSHIP_STATE_OWNED,
			ZEND_MIR_REFCOUNT_IMMORTAL)
		: result_kind == ZEND_TPDE_MACHINE_VALUE_BOOL
		? add_derived_value(
			ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_ID_INVALID, false, 0,
			ZEND_TPDE_MACHINE_VALUE_BOOL,
			ZEND_MIR_OWNERSHIP_STATE_OWNED,
			ZEND_MIR_REFCOUNT_IMMORTAL)
		: add_derived_value(
			ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_MIR_ID_INVALID, false, 0,
			ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			ZEND_MIR_OWNERSHIP_STATE_OWNED,
			ZEND_MIR_REFCOUNT_IMMORTAL);
	if (left == INVALID_VALUE_REF || right == INVALID_VALUE_REF
			|| !machine_value_has_register_definition(left)
			|| !machine_value_has_register_definition(right)) {
		valid_ = false;
		return;
	}
	const uint32_t operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.push_back(left);
	operands_.push_back(right);
	add_node(block_instructions, block, InstNode{
		.kind = InstKind::MIR,
		.mir_instruction_index = i,
		.argument_index = UINT32_MAX,
		.result = result,
		.operand_offset = operand_offset,
		.operand_count = 2,
		.has_result = true});
	set_instruction_result(i, result);
	active_source_ssa_overrides()[
		instruction.value_operation.result.ssa_variable_id] =
			result;
	override_ssa_value(
		instruction.value_operation.result.ssa_variable_id,
		result);
}

void ZendIRAdaptor::Builder::forward_typed_body_assign(
		uint32_t i, const zend_tpde_instruction &instruction, uint32_t block) {
	IRValueRef source =
		instruction.value_operation.op1.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL
			? source_operand_value_ref(
				instruction.value_operation.op1)
			: source_binding_value_ref(
				instruction.source_op1_binding);
	if (source == INVALID_VALUE_REF) {
		valid_ = false;
		return;
	}
	/* A boxed result, such as a phi of numbers, boxes a scalar
	 * source in this block. */
	const int32_t result_value = zend_tpde_value_index(plan_,
		zend_mir_value_from_original_ssa(
			instruction.value_operation.result.ssa_variable_id));
	if (result_value >= 0
			&& plan_->values[result_value].machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& machine_kind(source)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		const zend_mir_scalar_type_mask source_type =
			exact_type(source);
		const bool scalar =
			(source_type == ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(source)
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| (source_type == ZEND_MIR_SCALAR_TYPE_F64
				&& machine_kind(source)
					== ZEND_TPDE_MACHINE_VALUE_F64);
		const IRValueRef boxed = scalar
			? add_derived_value(ZEND_MIR_REPRESENTATION_ZVAL,
				ZEND_MIR_SCALAR_TYPE_NONE, ZEND_MIR_ID_INVALID,
				false, 0, ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ZEND_MIR_OWNERSHIP_STATE_OWNED,
				ZEND_MIR_REFCOUNT_IMMORTAL)
			: INVALID_VALUE_REF;
		if (boxed == INVALID_VALUE_REF) {
			valid_ = false;
			return;
		}
		const uint32_t box_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(source);
		add_node(block_instructions, block, InstNode{
			.kind = InstKind::BoxScalar,
			.mir_instruction_index = i,
			.argument_index = UINT32_MAX,
			.result = boxed,
			.operand_offset = box_offset,
			.operand_count = 1,
			.has_result = true,
			.storage_id = ZEND_MIR_ID_INVALID,
			.exact_type = source_type});
		source = boxed;
	}
	set_instruction_result(i, source);
	active_source_ssa_overrides()[
		instruction.value_operation.result.ssa_variable_id] =
			source;
	override_ssa_value(
		instruction.value_operation.result.ssa_variable_id,
		source);
}

void ZendIRAdaptor::Builder::lower_phi(InstContext &ctx) {
	if (ctx.result == INVALID_VALUE_REF) {
		valid_ = false;
		return;
	}
	auto phi_input_ref = [&](uint32_t operand) {
		const uint32_t operand_index =
			ctx.instruction.operand_offset + operand;
		if (operand_index
					< boxed_phi_input_overrides.size()
				&& boxed_phi_input_overrides[operand_index]
					!= INVALID_VALUE_REF) {
			return boxed_phi_input_overrides[operand_index];
		}
		const int32_t source_block = block_index(ctx.record.block_id);
		if (source_block < 0) {
			return INVALID_VALUE_REF;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[source_block];
		const uint32_t predecessor_count =
			plan_->block_predecessor_offsets[source_block + 1]
				- predecessor_begin;
		if (operand >= predecessor_count) {
			return INVALID_VALUE_REF;
		}
		const uint32_t predecessor = plan_->block_predecessors[
			predecessor_begin + operand];
		return predecessor < plan_->block_count
			? phi_edge_value_ref(zend_tpde_operand_at(
				plan_, &ctx.instruction, operand),
				final_blocks[predecessor])
			: INVALID_VALUE_REF;
	};
	if (representation(ctx.result) == ZEND_MIR_REPRESENTATION_ZVAL
			|| zend_tpde_machine_kind_is_pointer(machine_kind(ctx.result))) {
		/*
		 * An exact-null PHI carries neither payload nor type: every
		 * input is the same PHP value, and a two-part boxed PHI
		 * would have no machine use.
		 */
		if (exact_type(ctx.result)
				== ZEND_MIR_SCALAR_TYPE_NULL) {
			return;
		}
		const int32_t source_block =
			block_index(ctx.record.block_id);
		if (source_block < 0) {
			valid_ = false;
			return;
		}
		const uint32_t predecessor_begin =
			plan_->block_predecessor_offsets[source_block];
		const uint32_t predecessors =
			plan_->block_predecessor_offsets[source_block + 1]
				- predecessor_begin;
		const zend_mir_storage_id result_storage =
			canonical_storage(ctx.result);
		if (predecessors != ctx.instruction.operand_count) {
			valid_ = false;
			return;
		}
		const zend_tpde_machine_value_kind result_kind =
			machine_kind(ctx.result);
		const bool boxed_result =
			result_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		const bool pointer_result =
			zend_tpde_machine_kind_is_pointer(result_kind);
		/* A typed body's phi of numbers: its forwarded or computed
		 * inputs are defined later in plan order, so resolve them
		 * once the pass has defined them. */
		bool typed_number_phi = function_mode_
				== FunctionMode::TypedBody
			&& boxed_result;
		for (uint32_t n = 0; typed_number_phi
				&& n < predecessors; ++n) {
			const int32_t input_index = zend_tpde_value_index(
				plan_, zend_tpde_operand_at(
					plan_, &ctx.instruction, n));
			const int32_t producer = input_index >= 0
					&& plan_->source_value_definition_instructions
						!= nullptr
				? plan_->source_value_definition_instructions[
					input_index]
				: -1;
			typed_number_phi = producer >= 0
				&& static_cast<uint32_t>(producer)
					< plan_->instruction_count
				&& (zend_tpde_typed_forward_assign(
						plan_->instructions[producer])
					|| zend_tpde_typed_numeric_binary(plan_,
						plan_->instructions[producer]));
		}
		if (typed_number_phi) {
			/* The phi is a register zval of its own. */
			const IRValueRef canonical_phi = ctx.result;
			ctx.result = add_derived_value(
				ZEND_MIR_REPRESENTATION_ZVAL,
				ZEND_MIR_SCALAR_TYPE_NONE, ZEND_MIR_ID_INVALID,
				false, 0, ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ZEND_MIR_OWNERSHIP_STATE_OWNED,
				ZEND_MIR_REFCOUNT_IMMORTAL);
			active_value_overrides()[
				static_cast<uint32_t>(canonical_phi)
					- MIR_VALUE_BASE] = ctx.result;
			if (zend_mir_value_is_original_ssa(ctx.record.result_id)
					&& ctx.record.result_id
						< active_source_ssa_overrides().size()) {
				active_source_ssa_overrides()[ctx.record.result_id] =
					ctx.result;
			}
			set_instruction_result(ctx.i, ctx.result);
			block_phis.push_back(
				{static_cast<uint32_t>(ctx.block), ctx.result});
			phi_values_[static_cast<uint32_t>(ctx.result)] = 1;
			Slice &input_slice =
				phi_input_slices_[static_cast<uint32_t>(ctx.result)];
			input_slice.offset =
				static_cast<uint32_t>(phi_inputs_.size());
			for (uint32_t n = 0; n < predecessors; ++n) {
				const uint32_t predecessor_index = final_blocks[
					plan_->block_predecessors[
						predecessor_begin + n]];
				deferred_typed_phi_inputs.push_back({
					static_cast<uint32_t>(phi_inputs_.size()),
					zend_tpde_operand_at(plan_, &ctx.instruction, n)});
				phi_inputs_.push_back({INVALID_VALUE_REF,
					IRBlockRef{predecessor_index}});
				++input_slice.count;
			}
			return;
		}
		bool register_phi =
			boxed_result || pointer_result;
		bool shared_storage =
			zend_mir_id_is_valid(result_storage);
		for (uint32_t n = 0; n < predecessors; ++n) {
			const int32_t canonical_input_index =
				zend_tpde_value_index(plan_,
					zend_tpde_operand_at(
						plan_, &ctx.instruction, n));
			const IRValueRef canonical_input =
				canonical_input_index < 0
					? INVALID_VALUE_REF
					: IRValueRef{MIR_VALUE_BASE
						+ static_cast<uint32_t>(canonical_input_index)};
			const IRValueRef input = phi_input_ref(n);
			if (input == INVALID_VALUE_REF) {
				valid_ = false;
				register_phi = false;
				shared_storage = false;
				break;
			}
			register_phi &=
				machine_value_is_register_authoritative(input)
					&& machine_value_has_register_definition(input)
					&& machine_kind(input) == result_kind
					&& (pointer_result
						|| representation(input)
							== ZEND_MIR_REPRESENTATION_ZVAL);
			shared_storage &=
				canonical_input != INVALID_VALUE_REF
				&& canonical_storage(canonical_input)
					== result_storage;
		}
		if (register_phi) {
			block_phis.push_back(
				{static_cast<uint32_t>(ctx.block), ctx.result});
			phi_values_[
				static_cast<uint32_t>(ctx.result)] = 1;
			Slice &input_slice =
				phi_input_slices_[
					static_cast<uint32_t>(ctx.result)];
			input_slice.offset =
				static_cast<uint32_t>(phi_inputs_.size());
			for (uint32_t n = 0; n < predecessors; ++n) {
				const IRValueRef input = phi_input_ref(n);
				const uint32_t source_predecessor_index =
					plan_->block_predecessors[
						predecessor_begin + n];
				const uint32_t predecessor_index =
					final_blocks[source_predecessor_index];
				phi_inputs_.push_back({input,
					IRBlockRef{predecessor_index}});
				++input_slice.count;
				const uint32_t cold_predecessor =
					boxed_cond_cold_by_predecessor[
						source_predecessor_index];
				if (cold_predecessor != UINT32_MAX) {
					const uint32_t operand_index =
						ctx.instruction.operand_offset + n;
					const IRValueRef cold_input =
						operand_index
								< boxed_phi_cold_input_overrides.size()
							&& boxed_phi_cold_input_overrides[
								operand_index]
								!= INVALID_VALUE_REF
							? boxed_phi_cold_input_overrides[
								operand_index]
							: input;
					phi_inputs_.push_back(
						{cold_input,
							IRBlockRef{cold_predecessor}});
					++input_slice.count;
				}
			}
			return;
		}
		/*
		 * A registerless PHI is valid only when every identity is
		 * the same clean Zend slot.
		 */
	if (!shared_storage) {
		valid_ = false;
	}
		return;
	}
	if (!zend_mir_scalar_type_is_exact(exact_type(ctx.result))
			|| exact_type(ctx.result) == ZEND_MIR_SCALAR_TYPE_NULL) {
		return;
	}
	if (!machine_value_has_result_representation(ctx.result)) {
		const zend_mir_storage_id result_storage =
			canonical_storage(ctx.result);
		bool shared_storage =
			zend_mir_id_is_valid(result_storage);
		for (uint32_t n = 0;
				shared_storage && n < ctx.instruction.operand_count;
				++n) {
			const IRValueRef input = phi_input_ref(n);
			shared_storage = input != INVALID_VALUE_REF
				&& canonical_storage(input) == result_storage;
		}
		if (!shared_storage) {
			valid_ = false;
		}
		return;
	}
	block_phis.push_back(
		{static_cast<uint32_t>(ctx.block), ctx.result});
	phi_values_[static_cast<uint32_t>(ctx.result)] = 1;
	const int32_t source_block =
		block_index(ctx.record.block_id);
	if (source_block < 0) {
		valid_ = false;
		return;
	}
	const uint32_t predecessor_begin =
		plan_->block_predecessor_offsets[source_block];
	const uint32_t predecessors =
		plan_->block_predecessor_offsets[source_block + 1]
			- predecessor_begin;
	if (predecessors != ctx.instruction.operand_count) {
		valid_ = false;
		return;
	}
	Slice &input_slice =
		phi_input_slices_[static_cast<uint32_t>(ctx.result)];
	input_slice.offset =
		static_cast<uint32_t>(phi_inputs_.size());
	for (uint32_t n = 0; n < predecessors; ++n) {
		IRValueRef input = phi_input_ref(n);
		const int32_t input_index = zend_tpde_value_index(
			plan_, zend_tpde_operand_at(plan_, &ctx.instruction, n));
		if (input_index >= 0) {
			const IRValueRef materializable =
				resolve_materializable_scalar(
					static_cast<uint32_t>(input_index),
					final_blocks[plan_->block_predecessors[
						predecessor_begin + n]], UINT32_MAX, 0);
			if (materializable != INVALID_VALUE_REF) {
				input = materializable;
			}
		}
		if (input == INVALID_VALUE_REF) {
			valid_ = false;
			continue;
		}
		const uint32_t source_predecessor_index =
			plan_->block_predecessors[
				predecessor_begin + n];
		const uint32_t predecessor_index =
			final_blocks[source_predecessor_index];
		phi_inputs_.push_back(
			{input, IRBlockRef{predecessor_index}});
		++input_slice.count;
		const uint32_t cold_predecessor =
			boxed_cond_cold_by_predecessor[
				source_predecessor_index];
		if (cold_predecessor != UINT32_MAX) {
			const uint32_t operand_index =
				ctx.instruction.operand_offset + n;
			const IRValueRef cold_input =
				operand_index
						< boxed_phi_cold_input_overrides.size()
					&& boxed_phi_cold_input_overrides[
						operand_index] != INVALID_VALUE_REF
				? boxed_phi_cold_input_overrides[operand_index]
				: input;
			phi_inputs_.push_back(
				{cold_input, IRBlockRef{cold_predecessor}});
			++input_slice.count;
		}
	}
}

void ZendIRAdaptor::Builder::append_data_operands(InstContext &ctx) {
	const uint32_t semantic_prefix_end =
		static_cast<uint32_t>(operands_.size());
	std::vector<IRValueRef> data_operands;
	data_operands.reserve(ctx.data_operand_count);
	bool data_operand_helpers = false;
	for (uint32_t n = 0; n < ctx.data_operand_count; ++n) {
		IRValueRef operand = value_ref(zend_tpde_operand_at(
			plan_, &ctx.instruction, n));
		const uint32_t transport_index =
			ctx.instruction.operand_offset + n;
		const zend_tpde_operand_transport *transport =
			plan_->instruction_operand_transports != nullptr
				&& transport_index
					< plan_->instruction_operand_count
			? &plan_->instruction_operand_transports[
				transport_index]
				: nullptr;
		const bool canonical_scalar_operand =
			transport != nullptr
			&& transport->kind
				== ZEND_TPDE_OPERAND_TRANSPORT_CANONICAL_SCALAR_LOAD;
		if (transport != nullptr
				&& transport->kind
					== ZEND_TPDE_OPERAND_TRANSPORT_DIRECT
				&& operand != INVALID_VALUE_REF
				&& !machine_value_is_register_authoritative(operand)) {
			const uint32_t raw = static_cast<uint32_t>(operand);
			if (is_mir_value(raw)) {
				const IRValueRef materializable =
					resolve_materializable_scalar(
						raw - MIR_VALUE_BASE, ctx.block, ctx.i, 0);
				if (materializable != INVALID_VALUE_REF) {
					operand = materializable;
				}
			}
		}
		if (canonical_scalar_operand) {
			uint64_t constant_bits;
			if (!constant(operand, &constant_bits)) {
				/*
				 * Representation selection decided this canonical-slot
				 * transition; the source opcode is not reinterpreted
				 * here.
				 */
				const zend_mir_storage_id storage_id =
					transport->storage_id;
				const IRValueRef address =
					frame_slot_address(storage_id);
				const IRValueRef resolved =
					address != INVALID_VALUE_REF
						&& transport->resolve_reference
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							storage_id, false, 0, UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN)
						: address;
				const IRValueRef transported =
					resolved != INVALID_VALUE_REF
						? add_derived_value(
							transport->representation,
							transport->exact_type,
							storage_id, false, 0,
							transport->machine_kind,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_IMMORTAL)
						: INVALID_VALUE_REF;
				if (transported == INVALID_VALUE_REF) {
					valid_ = false;
				} else {
					uint32_t load_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(address);
					data_operand_helpers = true;
					if (transport->resolve_reference) {
						add_node(block_instructions, ctx.block, InstNode{
							.kind = InstKind::ZvalReferenceResolve,
							.mir_instruction_index = ctx.i,
							.argument_index = UINT32_MAX,
							.result = resolved,
							.operand_offset = load_operand_offset,
							.operand_count = 1,
							.has_result = true,
							.storage_id = storage_id,
							.exact_type = ZEND_MIR_SCALAR_TYPE_NONE});
					}
					load_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(resolved);
					add_node(block_instructions, ctx.block, InstNode{
						.kind = InstKind::ZvalPayloadLoad,
						.mir_instruction_index = ctx.i,
						.argument_index = UINT32_MAX,
						.result = transported,
						.operand_offset = load_operand_offset,
						.operand_count = 1,
						.has_result = true,
						.storage_id = storage_id,
						.exact_type = transport->exact_type});
					operand = transported;
				}
			}
		}
		if (ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& n == 0
				&& operand != INVALID_VALUE_REF
				&& !machine_value_is_register_authoritative(operand)) {
			const uint32_t operand_index =
				static_cast<uint32_t>(operand);
			if (is_mir_value(operand_index)
					&& plan_->value_definition_instructions
						!= nullptr) {
				const int32_t definition =
					plan_->value_definition_instructions[
						operand_index - MIR_VALUE_BASE];
				if (definition >= 0
						&& static_cast<uint32_t>(definition)
							< plan_->instruction_count) {
					const zend_tpde_instruction &defining =
						plan_->instructions[
							static_cast<uint32_t>(definition)];
					const zend_mir_instruction_record
						defining_record =
							instruction_record_at(
								static_cast<uint32_t>(definition));
					if ((defining_record.opcode
								== ZEND_MIR_OPCODE_COPY
							|| defining_record.opcode
								== ZEND_MIR_OPCODE_CANONICALIZE)
							&& defining.operand_count == 1) {
						const IRValueRef input = value_ref(
							zend_tpde_operand_at(
								plan_, &defining, 0));
						uint64_t constant_bits;
						if (input != INVALID_VALUE_REF
								&& representation(input)
									== representation(operand)
								&& exact_type(input)
									== exact_type(operand)
								&& (constant(input, &constant_bits)
									|| (machine_value_is_register_authoritative(
											input)
										&& machine_value_has_register_definition(
											input)))) {
							/*
							 * A registerless COPY/CANONICALIZE can feed
							 * the following ZVAL_STORE: its machine
							 * input is stored, as reloading the
							 * destination slot would read the old
							 * payload.
							 */
							operand = input;
						}
					}
				}
			}
		}
		if (ctx.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& n == 0
				&& operand != INVALID_VALUE_REF
				&& !machine_value_is_register_authoritative(operand)
				&& !machine_value_has_register_definition(operand)
				&& zend_mir_scalar_type_is_exact(
					exact_type(operand))
				&& exact_type(operand)
					!= ZEND_MIR_SCALAR_TYPE_NULL) {
			uint64_t constant_bits;
			if (!constant(operand, &constant_bits)) {
				/*
				 * A slot-authoritative scalar has no TPDE definition of
				 * its own, but ZVAL_STORE needs the old payload after
				 * its release slow path: it is captured before the
				 * fast/cold split and becomes an SSA operand of both
				 * branches.
				 */
				const zend_mir_storage_id storage_id =
					canonical_storage(operand);
				const IRValueRef address =
					frame_slot_address(storage_id);
				const IRValueRef transported =
					address != INVALID_VALUE_REF
						? add_derived_value(
							representation(operand),
							exact_type(operand),
							storage_id, false, 0,
							machine_kind(operand),
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							refcount_state(operand))
						: INVALID_VALUE_REF;
				if (transported == INVALID_VALUE_REF) {
					valid_ = false;
				} else {
					const uint32_t load_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(address);
					data_operand_helpers = true;
					add_node(block_instructions, ctx.block, InstNode{
						.kind = InstKind::ZvalPayloadLoad,
						.mir_instruction_index = ctx.i,
						.argument_index = UINT32_MAX,
						.result = transported,
						.operand_offset = load_operand_offset,
						.operand_count = 1,
						.has_result = true,
						.storage_id = storage_id,
						.exact_type = exact_type(operand)});
					operand = transported;
				}
			}
		}
		if (operand == INVALID_VALUE_REF) {
			valid_ = false;
		}
		data_operands.push_back(operand);
	}
	if (data_operand_helpers) {
		move_operand_prefix(ctx.operand_offset, semantic_prefix_end);
	}
	operands_.insert(operands_.end(),
		data_operands.begin(), data_operands.end());
}

void ZendIRAdaptor::Builder::add_return_source_operands(
		uint32_t i, const zend_tpde_instruction &instruction) {
	IRValueRef returned = source_binding_value_ref(
		instruction.source_op1_binding);
	/* A typed body elides the COPY of a returned pi, such as
	 * $value after if ($value > $max); return its source. */
	for (uint32_t depth = 0;
			function_mode_ == FunctionMode::TypedBody
				&& returned != INVALID_VALUE_REF
				&& !machine_value_has_register_definition(returned)
				&& depth < plan_->value_count;
			++depth) {
		const uint32_t raw = static_cast<uint32_t>(returned);
		if (!is_mir_value(raw)
				|| plan_->value_definition_instructions == nullptr) {
			break;
		}
		const int32_t definition =
			plan_->value_definition_instructions[
				raw - MIR_VALUE_BASE];
		if (definition < 0 || static_cast<uint32_t>(definition)
					>= plan_->instruction_count) {
			break;
		}
		const zend_tpde_instruction &copy =
			plan_->instructions[static_cast<uint32_t>(definition)];
		if (instruction_record_at(
					static_cast<uint32_t>(definition)).opcode
				!= ZEND_MIR_OPCODE_COPY
				|| copy.operand_count != 1) {
			break;
		}
		returned = value_ref(zend_tpde_operand_at(
			plan_, &copy, 0));
	}
	const uint32_t return_source_position =
		instruction.value_operation.source_position_id;
	const uint8_t return_producer_opcode =
		return_source_position != 0
			&& return_source_position <= plan_->source_opcode_count
		? plan_->source_opcodes[return_source_position - 1].opcode
		: ZEND_NOP;
	const bool immediate_call_result =
		return_producer_opcode == ZEND_DO_UCALL
		|| return_producer_opcode == ZEND_DO_FCALL
		|| return_producer_opcode == ZEND_DO_FCALL_BY_NAME
		|| return_producer_opcode == ZEND_DO_ICALL;
	const int32_t return_definition =
		instruction.source_op1_binding
			.definition_instruction_index;
	const bool immediate_register_call_result =
		return_definition >= 0
		&& static_cast<uint32_t>(return_definition)
			< plan_->instruction_count
		&& (frozen_typed_component_call(
				static_cast<uint32_t>(return_definition))
			|| frozen_effect_closed_inline(
				static_cast<uint32_t>(return_definition)));
	/*
	 * An ordinary Zend-entry call writes its canonical result slot,
	 * and with optimized TMP reuse the register may still hold that
	 * slot's previous producer. A local-ABI call defines the
	 * register result directly, in a typed body and at Zend entry.
	 */
	bool stale_canonical_return = false;
	if (function_mode_ == FunctionMode::ZendEntry
			&& returned != INVALID_VALUE_REF
			&& zend_mir_id_is_valid(
				instruction.value_operation.op1_storage_id)) {
		const uint32_t begin =
			instruction.source_op1_binding
						.definition_instruction_index < 0
				? 0
				: static_cast<uint32_t>(
					instruction.source_op1_binding
						.definition_instruction_index + 1);
		const std::vector<uint32_t> &writers =
			canonical_slot_writers(
				instruction.value_operation.op1_storage_id);
		const auto writer =
			std::ranges::lower_bound(writers, begin);
		stale_canonical_return =
			writer != writers.end() && *writer < i;
	}
	if (function_mode_ == FunctionMode::TypedBody
			&& returned == INVALID_VALUE_REF) {
		valid_ = false;
	}
	if (instruction.value_operation.source_opcode == ZEND_RETURN
		&& returned != INVALID_VALUE_REF
		&& (function_mode_ == FunctionMode::TypedBody
				|| !immediate_call_result
				|| immediate_register_call_result)
			&& !stale_canonical_return
			/*
			 * ARRAY_PTR values in a Zend entry may be borrowed
			 * views of the canonical result slot: returning the
			 * pointer would copy a TMP/VAR source instead of
			 * transferring and undefining its zval, so those
			 * returns take the ownership-aware helper. A typed body
			 * has no canonical frame and returns its ABI value.
			 */
			&& ((function_mode_ == FunctionMode::TypedBody)
				|| machine_kind(returned)
					!= ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR)
			&& machine_value_has_register_definition(
				returned)) {
		operands_.push_back(returned);
	}
}

void ZendIRAdaptor::Builder::add_user_call_operands(InstContext &ctx) {
	const bool typed_local_call =
		frozen_typed_component_call(ctx.i);
	if (typed_local_call) {
		add_typed_call_operands(ctx);
	} else {
		add_zend_frame_call_operands(ctx);
	}
}

void ZendIRAdaptor::Builder::add_typed_call_operands(InstContext &ctx) {
	std::vector<IRValueRef> typed_call_values;
	typed_call_values.reserve(
		ctx.instruction.call_argument_count);
	for (uint32_t n = 0;
			n < ctx.instruction.call_argument_count; ++n) {
		add_typed_call_argument(ctx, n, typed_call_values);
	}
	ctx.operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.insert(operands_.end(),
		typed_call_values.begin(),
		typed_call_values.end());
	if (function_mode_ == FunctionMode::ZendEntry) {
		operands_.push_back(
			IRValueRef{FRAME_VALUE});
		operands_.push_back(
			IRValueRef{FRAME_VALUE});
		ctx.direct_call_context_operand =
			static_cast<uint32_t>(operands_.size())
				- ctx.operand_offset;
		for (uint32_t use = 0; use < 3; ++use) {
			operands_.push_back(
				IRValueRef{
					EXECUTION_CONTEXT_ARGUMENT});
		}
	}
}

void ZendIRAdaptor::Builder::add_typed_call_argument(InstContext &ctx,
		uint32_t n, std::vector<IRValueRef> &typed_call_values) {
	zend_mir_call_argument_ref argument{};
	if (!zend_tpde_call_argument_at(plan_,
			ctx.instruction.call_argument_offset + n,
			&argument)) {
		valid_ = false;
		typed_call_values.push_back(
			INVALID_VALUE_REF);
		return;
	}
	const uint32_t call_argument_index =
		ctx.instruction.call_argument_offset + n;
	const zend_tpde_source_value_binding &argument_binding =
		plan_->call_argument_bindings[call_argument_index];
	const int32_t binding_definition =
		argument_binding.definition_instruction_index;
	const bool binding_stayed_canonical =
		binding_definition >= 0
		&& static_cast<uint32_t>(binding_definition)
			< active_instruction_results().size()
		&& active_instruction_results()[
			static_cast<uint32_t>(binding_definition)]
			== INVALID_VALUE_REF;
	IRValueRef value = source_binding_value_ref(
		argument_binding);
	if (value == INVALID_VALUE_REF) {
		value = source_operand_value_ref(
			argument.source_operand);
	}
	const zend_tpde_plan *callee =
		ctx.instruction.component_target_index
				< component_plans_.size()
			? component_plans_[
				ctx.instruction.component_target_index]
			: nullptr;
	const int32_t callee_value =
		callee != nullptr
				&& callee->argument_value_indices != nullptr
				&& n < callee->argument_count
			? callee->argument_value_indices[n] : -1;
	const TypedBodyAbiType transport_abi =
		callee_value >= 0
				&& static_cast<uint32_t>(callee_value)
					< callee->value_count
			? typed_body_value_abi(
				callee,
				static_cast<uint32_t>(callee_value))
			: TypedBodyAbiType{};
	/*
	 * A Zend-entry value may stay slot-authoritative until
	 * a local-ABI call needs its machine representation:
	 * scalars, pointers and boxed values are materialized
	 * only in the observer-free hot block, and the cold
	 * call reads the frame.
	 */
	const bool transport_scalar =
		transport_abi.valid
		&& zend_mir_scalar_type_is_exact(
			transport_abi.exact_type)
		&& transport_abi.exact_type
			!= ZEND_MIR_SCALAR_TYPE_NULL;
	const bool transport_pointer =
		transport_abi.valid
		&& zend_tpde_machine_kind_is_pointer(
			transport_abi.machine_kind);
	const bool transport_boxed =
		transport_abi.valid
		&& transport_abi.machine_kind
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	const bool matching_register_value =
		!binding_stayed_canonical
		&& value != INVALID_VALUE_REF
		&& representation(value)
			== transport_abi.representation
		&& exact_type(value)
			== transport_abi.exact_type
		&& machine_kind(value)
			== transport_abi.machine_kind
		&& machine_value_is_register_authoritative(
			value)
		&& machine_value_has_register_definition(value);
	const bool guarded_boxed_pointer =
		function_mode_ == FunctionMode::ZendEntry
		&& !binding_stayed_canonical
		&& transport_pointer
		&& value != INVALID_VALUE_REF
		&& representation(value)
			== ZEND_MIR_REPRESENTATION_ZVAL
		&& machine_kind(value)
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
		&& ownership(value)
			== ZEND_MIR_OWNERSHIP_STATE_OWNED
		&& machine_value_is_register_authoritative(value)
		&& machine_value_has_register_definition(value)
		&& n < ctx.instruction.direct_call->argument_count
		&& ctx.instruction.direct_call->arguments[n].exact_type
			== transport_abi.exact_type
		&& guarded_hot_blocks[ctx.i] != UINT32_MAX
		&& guarded_cold_blocks[ctx.i] != UINT32_MAX;
	const bool guarded_boxed_integer =
		function_mode_ == FunctionMode::ZendEntry
		&& !binding_stayed_canonical
		&& transport_scalar
		&& transport_abi.representation
			== ZEND_MIR_REPRESENTATION_I64
		&& transport_abi.exact_type
			== ZEND_MIR_SCALAR_TYPE_I64
		&& transport_abi.machine_kind
			== ZEND_TPDE_MACHINE_VALUE_I64
		&& value != INVALID_VALUE_REF
		&& representation(value)
			== ZEND_MIR_REPRESENTATION_ZVAL
		&& machine_kind(value)
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
		&& zend_mir_id_is_valid(canonical_storage(value))
		&& machine_value_is_register_authoritative(value)
		&& machine_value_has_register_definition(value)
		&& n < ctx.instruction.direct_call->argument_count
		&& ctx.instruction.direct_call->arguments[n].mode
			== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
		&& ctx.instruction.direct_call->arguments[n].exact_type
			== ZEND_MIR_SCALAR_TYPE_I64
			&& ctx.instruction.direct_call_argument_guard_types != nullptr
			&& ctx.instruction.direct_call_argument_guard_types[n]
				== ZEND_MIR_SCALAR_TYPE_I64
			&& guarded_hot_blocks[ctx.i] != UINT32_MAX
			&& guarded_cold_blocks[ctx.i] != UINT32_MAX;
	if (guarded_boxed_pointer) {
		const IRValueRef boxed = value;
		const IRValueRef transported = add_derived_value(
			transport_abi.representation,
			transport_abi.exact_type,
			canonical_storage(boxed), false, 0,
			transport_abi.machine_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_UNKNOWN);
		const uint32_t unbox_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(boxed);
		add_node(block_instructions,
			guarded_hot_blocks[ctx.i], InstNode{
				.kind = InstKind::UnboxPointer,
				.mir_instruction_index = ctx.i,
				.argument_index = UINT32_MAX,
				.result = transported,
				.operand_offset = unbox_operand_offset,
				.operand_count = 1,
				.has_result = true,
				.storage_id = canonical_storage(boxed),
				.exact_type = transport_abi.exact_type});
		const uint32_t expected_zval_type =
			zend_tpde_machine_value_zval_type(
				transport_abi.machine_kind);
		if (expected_zval_type == IS_UNDEF) {
			valid_ = false;
		} else {
			ctx.typed_call_value_guards.emplace_back(
				boxed, expected_zval_type);
			ctx.typed_call_owned_boxed_arguments.push_back(
				boxed);
			value = transported;
		}
	}
	if (guarded_boxed_integer) {
		const IRValueRef boxed = value;
		const IRValueRef transported = add_derived_value(
			transport_abi.representation,
			transport_abi.exact_type,
			canonical_storage(boxed), false, 0,
			transport_abi.machine_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_IMMORTAL);
		const uint32_t unbox_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(boxed);
		add_node(block_instructions,
			guarded_hot_blocks[ctx.i], InstNode{
				.kind = InstKind::UnboxScalar,
				.mir_instruction_index = ctx.i,
				.argument_index = UINT32_MAX,
				.result = transported,
				.operand_offset = unbox_operand_offset,
				.operand_count = 1,
				.has_result = true,
				.storage_id = canonical_storage(boxed),
				.exact_type = transport_abi.exact_type});
		ctx.typed_call_value_guards.emplace_back(
			boxed, IS_LONG);
		value = transported;
	}
	/* A register or constant scalar is boxed for a zval
	 * parameter. */
	uint64_t box_constant_bits;
	const bool box_scalar =
		!binding_stayed_canonical
		&& transport_boxed
		&& value != INVALID_VALUE_REF
		&& !guarded_boxed_pointer
		&& !guarded_boxed_integer
		&& zend_tpde_abi_boxes_into(
			TypedBodyAbiType{representation(value),
				exact_type(value), machine_kind(value),
				ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE, true},
			transport_abi)
		&& (constant(value, &box_constant_bits)
			|| (machine_value_is_register_authoritative(
					value)
				&& machine_value_has_register_definition(
					value)));
	if (box_scalar) {
		const IRValueRef boxed = add_derived_value(
			ZEND_MIR_REPRESENTATION_ZVAL,
			transport_abi.exact_type, ZEND_MIR_ID_INVALID,
			false, 0, ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			ZEND_MIR_REFCOUNT_IMMORTAL);
		const uint32_t box_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(value);
		add_node(block_instructions,
			guarded_hot_blocks[ctx.i] != UINT32_MAX
				? guarded_hot_blocks[ctx.i]
				: static_cast<uint32_t>(ctx.block),
			InstNode{
				.kind = InstKind::BoxScalar,
				.mir_instruction_index = ctx.i,
				.argument_index = UINT32_MAX,
				.result = boxed,
				.operand_offset = box_operand_offset,
				.operand_count = 1,
				.has_result = true,
				.storage_id = ZEND_MIR_ID_INVALID,
				.exact_type = exact_type(value)});
		value = boxed;
	}
	if (function_mode_ == FunctionMode::ZendEntry
			&& (transport_scalar
				|| transport_pointer
				|| transport_boxed)
			&& !matching_register_value
			&& !guarded_boxed_pointer
			&& !guarded_boxed_integer
			&& !box_scalar) {
		zend_mir_storage_id storage_id =
			canonical_storage(value);
		if (!zend_mir_id_is_valid(storage_id)) {
			storage_id = source_operand_storage_id(
				argument.source_operand);
		}
		const IRValueRef address =
			frame_slot_address(storage_id);
		const IRValueRef transported =
			address != INVALID_VALUE_REF
				? add_derived_value(
					transport_abi.representation,
					transport_abi.exact_type,
					storage_id, false, 0,
					transport_abi.machine_kind,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN)
				: INVALID_VALUE_REF;
		if (transported != INVALID_VALUE_REF) {
			const uint32_t load_operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(address);
			const uint32_t transport_block =
				guarded_hot_blocks[ctx.i] != UINT32_MAX
					? guarded_hot_blocks[ctx.i]
					: static_cast<uint32_t>(ctx.block);
			add_node(block_instructions,
				transport_block, InstNode{
					.kind = InstKind::ZvalPayloadLoad,
					.mir_instruction_index = ctx.i,
					.argument_index = UINT32_MAX,
					.result = transported,
					.operand_offset = load_operand_offset,
					.operand_count = 1,
					.has_result = true,
					.storage_id = storage_id,
					.exact_type = transport_abi.exact_type});
			value = transported;
		}
	}
	if (!machine_value_has_result_representation(value)
			|| !machine_value_has_register_definition(value)) {
		valid_ = false;
	}
	typed_call_values.push_back(value);
}

void ZendIRAdaptor::Builder::add_zend_frame_call_operands(InstContext &ctx) {
	/*
	 * A proven inline descriptor materializes exact scalar payloads
	 * and boxed zvals in the generated Zend frame. Repeated frame
	 * and context uses stay explicit so TPDE's reference counts
	 * match both paths; a non-inline boxed result uses one more
	 * caller-frame reference to load both value parts after the
	 * call.
	 */
	const bool dynamic_direct_call =
		ctx.instruction.direct_call == nullptr
		&& ctx.instruction.user_call != nullptr
		&& ctx.instruction.user_call->do_opcode
			!= ZEND_CALLABLE_CONVERT
		&& ctx.instruction.user_call->do_opcode
			!= ZEND_CALLABLE_CONVERT_PARTIAL;
	uint32_t frame_use_count;
	if (ctx.instruction.direct_call != nullptr) {
		if ((ctx.instruction.direct_call->flags
				& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0) {
			for (uint32_t n = 0;
					n < ctx.instruction.call_argument_count; ++n) {
				zend_mir_call_argument_ref argument;
				if (!zend_tpde_call_argument_at(plan_,
						ctx.instruction.call_argument_offset + n,
						&argument)) {
					valid_ = false;
					operands_.push_back(INVALID_VALUE_REF);
					continue;
				}
				IRValueRef value = source_binding_value_ref(
					plan_->call_argument_bindings[
						ctx.instruction.call_argument_offset + n]);
				if (value == INVALID_VALUE_REF
						&& zend_mir_id_is_valid(
							argument.value_id)) {
					value = value_ref(argument.value_id);
				}
				if (value != INVALID_VALUE_REF
						&& (!machine_value_has_result_representation(
								value)
							|| !machine_value_has_register_definition(
								value))) {
					/*
					 * A canonical-only source is not a machine
					 * value, nor is one without a machine
					 * definition, such as the pi of a CV narrowed
					 * by a comparison: the generated frame path
					 * copies the zval from the caller frame at its
					 * byte offset.
					 */
					value = IRValueRef{FRAME_VALUE};
				}
				if (value == INVALID_VALUE_REF) {
					value = IRValueRef{FRAME_VALUE};
				}
				if (value == INVALID_VALUE_REF) {
					valid_ = false;
				}
				operands_.push_back(value);
			}
			const bool leaf_scalar_frame =
				(ctx.instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME) != 0;
			frame_use_count = zend_tpde_direct_user_call_frame_uses(
				leaf_scalar_frame, ctx.machine_result,
				!leaf_scalar_frame
					&& (ctx.instruction.direct_call->expected_function
						->common.fn_flags & ZEND_ACC_VARIADIC) != 0);
		} else {
			frame_use_count = 2
				+ (ctx.machine_result
					&& machine_kind(ctx.result)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		}
	} else if (dynamic_direct_call) {
		frame_use_count = 2;
	} else {
		uint32_t setter_count = ctx.instruction.operand_count == 0
			? ctx.instruction.call_argument_count
			: ctx.instruction.operand_count;
		frame_use_count = setter_count + 2 + ctx.machine_result;
	}
	for (uint32_t n = 0; n < frame_use_count; ++n) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
	if (ctx.instruction.direct_call != nullptr) {
		ctx.direct_call_context_operand =
			static_cast<uint32_t>(operands_.size())
				- ctx.operand_offset;
		const uint32_t context_use_count =
			zend_tpde_direct_user_call_context_uses(
				(ctx.instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0,
				(ctx.instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME) != 0,
				ctx.inlined_user_body.valid, ctx.machine_result);
		for (uint32_t n = 0; n < context_use_count; ++n) {
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		}
		if (ctx.inlined_user_body.valid) {
			if (ctx.inlined_user_body.checked()) {
				operands_.insert(operands_.end(),
					ctx.inlined_user_body.checked_operands.begin(),
					ctx.inlined_user_body.checked_operands.end());
			} else {
				operands_.push_back(
					ctx.inlined_user_body.value);
			}
		}
	} else if (dynamic_direct_call) {
		for (uint32_t n = 0; n < 3; ++n) {
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		}
	}
}

void ZendIRAdaptor::Builder::add_internal_call_operands(InstContext &ctx) {
	std::vector<IRValueRef> argument_operands;
	argument_operands.reserve(ctx.instruction.call_argument_count);
	ctx.direct_internal_argument_transport =
		function_mode_ == FunctionMode::ZendEntry
		&& ctx.instruction.direct_internal_call != nullptr
		&& plan_->generator_resume_count == 0
		&& ctx.instruction.call_argument_count != 0;
	for (uint32_t n = 0;
			ctx.direct_internal_argument_transport
				&& n < ctx.instruction.call_argument_count; ++n) {
		const uint32_t argument_index =
			ctx.instruction.call_argument_offset + n;
		zend_mir_call_argument_ref argument{};
		if (argument_index >= plan_->call_argument_count
				|| !zend_tpde_call_argument_at(
					plan_, argument_index, &argument)) {
			ctx.direct_internal_argument_transport = false;
			break;
		}
		/*
		 * A source operation may replace the plan value with a
		 * register result after the call binding was fixed. The
		 * exact SEND SSA version wins, so an immediately consumed
		 * scalar (strlen($value) passed to var_dump()) is placed
		 * from that register, not from its still undefined
		 * temporary slot.
		 */
		const zend_tpde_source_value_binding &binding =
			plan_->call_argument_bindings[argument_index];
		IRValueRef value = source_binding_value_ref(binding);
		if (!machine_value_has_result_representation(value)
				&& ctx.instruction.materialization_offset
					<= plan_->materialization_count
				&& ctx.instruction.materialization_count
					<= plan_->materialization_count
						- ctx.instruction.materialization_offset) {
			for (uint32_t materialization_index = 0;
					materialization_index
						< ctx.instruction.materialization_count;
					++materialization_index) {
				const zend_tpde_materialization &materialization =
					plan_->materializations[
						ctx.instruction.materialization_offset
							+ materialization_index];
				if (materialization.value_index != UINT32_MAX
						|| materialization.source_value_index
							!= binding.value_index
						|| materialization
							.source_definition_instruction_index < 0) {
					continue;
				}
				const IRValueRef materialized =
					source_binding_value_ref({
						materialization.source_value_index,
						materialization
							.source_definition_instruction_index,
					});
				if (machine_value_has_result_representation(
						materialized)) {
					value = materialized;
					break;
				}
			}
		}
		if (value == INVALID_VALUE_REF
				&& zend_mir_id_is_valid(argument.value_id)) {
			value = value_ref(argument.value_id);
		}
		if (value == INVALID_VALUE_REF) {
			value = source_operand_value_ref(
				argument.source_operand);
		}
		const zend_mir_scalar_type_mask type =
			value != INVALID_VALUE_REF
				? exact_type(value)
				: ZEND_MIR_SCALAR_TYPE_NONE;
		const zend_tpde_machine_value_kind kind =
			value != INVALID_VALUE_REF
				? machine_kind(value)
				: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		const bool direct_scalar_representation =
			(type == ZEND_MIR_SCALAR_TYPE_I1
				&& kind == ZEND_TPDE_MACHINE_VALUE_BOOL)
			|| (type == ZEND_MIR_SCALAR_TYPE_I64
				&& kind == ZEND_TPDE_MACHINE_VALUE_I64)
			|| (type == ZEND_MIR_SCALAR_TYPE_F64
				&& kind == ZEND_TPDE_MACHINE_VALUE_F64);
		const bool direct_scalar =
			ctx.instruction.direct_internal_call->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& value != INVALID_VALUE_REF
			&& machine_value_has_result_representation(value)
			&& (type == ZEND_MIR_SCALAR_TYPE_NULL
				|| machine_value_has_register_definition(value))
			&& direct_scalar_representation
			&& zend_mir_scalar_type_is_exact(type)
			&& (type == ZEND_MIR_SCALAR_TYPE_NULL
				|| type == ZEND_MIR_SCALAR_TYPE_I1
				|| type == ZEND_MIR_SCALAR_TYPE_I64
				|| type == ZEND_MIR_SCALAR_TYPE_F64);
		const bool direct_boxed_temporary =
			ctx.instruction.direct_internal_call->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			&& value != INVALID_VALUE_REF
			&& machine_value_has_result_representation(value)
			&& machine_value_has_register_definition(value)
			&& kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& (argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| argument.source_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (argument.source_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
				|| argument.source_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_VAR)
			&& zend_mir_id_is_valid(
				source_operand_storage_id(argument.source_operand));
		const bool stable_source =
			direct_internal_source_argument_stable(
				argument, ctx.record.source_position_id);
		if (direct_scalar || direct_boxed_temporary) {
			argument_operands.push_back(value);
			if (direct_boxed_temporary) {
				++ctx.direct_internal_boxed_argument_count;
			}
			continue;
		}
		/*
		 * Literals and source slots proven untouched after their
		 * SEND can be decoded after the other arguments. A reused
		 * slot needs an exact transported SSA scalar above, or the
		 * whole call takes the conservative path.
		 */
		if (ctx.instruction.direct_internal_call->arguments[n].mode
				== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
				&& (argument.source_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| stable_source)) {
			argument_operands.push_back(IRValueRef{FRAME_VALUE});
			continue;
		}
		ctx.direct_internal_argument_transport = false;
		break;
	}
	if (ctx.direct_internal_argument_transport) {
		operands_.insert(operands_.end(),
			argument_operands.begin(), argument_operands.end());
		/*
		 * begin, each setter, finish and an optional result read
		 * are separate calls: every frame use stays explicit for
		 * TPDE liveness across them.
		 */
		const uint32_t frame_uses =
			ctx.instruction.call_argument_count + 2
			+ (ctx.machine_result ? 1 : 0)
			+ ctx.direct_internal_boxed_argument_count;
		for (uint32_t n = 0; n < frame_uses; ++n) {
			operands_.push_back(IRValueRef{FRAME_VALUE});
		}
	} else {
		/* One direct Zend-runtime boundary returns status and payload. */
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
}

void ZendIRAdaptor::Builder::add_materialization_operands(
		InstContext &ctx) {
	if (ctx.instruction.materialization_offset
				> plan_->materialization_count
			|| ctx.instruction.materialization_count
				> plan_->materialization_count
					- ctx.instruction.materialization_offset) {
		valid_ = false;
	} else {
		for (uint32_t n = 0;
				n < ctx.materialization_count; ++n) {
			const zend_tpde_materialization &materialization =
				plan_->materializations[
					ctx.instruction.materialization_offset + n];
			IRValueRef value = INVALID_VALUE_REF;
			if (materialization.value_index < plan_->value_count) {
				value = resolve_materializable_scalar(
					materialization.value_index, ctx.block, ctx.i, 0);
				if (value == INVALID_VALUE_REF
						|| !machine_value_has_result_representation(value)
						|| !machine_value_has_register_definition(value)) {
					const IRValueRef canonical{
						MIR_VALUE_BASE + materialization.value_index};
					const zend_mir_storage_id source_storage =
						canonical_storage(canonical);
					/* Reloading a slot only to store it back is no
					 * materialization; the frame stands in for it. */
					if (zend_mir_id_is_valid(source_storage)
							&& source_storage
								== materialization.storage_id) {
						operands_.push_back(IRValueRef{FRAME_VALUE});
						++ctx.operand_count;
						continue;
					}
					const IRValueRef address = frame_slot_address(source_storage);
					const IRValueRef loaded = address != INVALID_VALUE_REF
						? add_derived_value(
							representation(canonical), exact_type(canonical),
							source_storage, false, 0,
							machine_kind(canonical),
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							refcount_state(canonical))
						: INVALID_VALUE_REF;
					if (loaded != INVALID_VALUE_REF) {
						add_payload_load_before(block_instructions,
							ctx.block, ctx.i, address, loaded, source_storage,
							exact_type(canonical), ctx.operand_offset);
						value = loaded;
					}
				}
			} else if (materialization.value_index == UINT32_MAX) {
				value = source_binding_value_ref({
					materialization.source_value_index,
					materialization
						.source_definition_instruction_index,
				});
				if ((value == INVALID_VALUE_REF
						|| !machine_value_has_result_representation(value))
						&& materialization.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						&& materialization
							.source_definition_instruction_index >= 0
						&& static_cast<uint32_t>(materialization
							.source_definition_instruction_index)
							< plan_->instruction_count) {
					const zend_tpde_instruction &definition =
						plan_->instructions[static_cast<uint32_t>(
							materialization
								.source_definition_instruction_index)];
					const uint32_t reference =
						definition.has_value_operation
							&& definition.value_operation.source_opcode
								== ZEND_STRLEN
						? machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							materialization.storage_id)
						: UINT32_MAX;
					const IRValueRef address = reference != UINT32_MAX
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							materialization.storage_id, false, 0,
							UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN, reference)
						: INVALID_VALUE_REF;
					const IRValueRef loaded = address != INVALID_VALUE_REF
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_I64,
							ZEND_MIR_SCALAR_TYPE_I64,
							materialization.storage_id, false, 0,
							ZEND_TPDE_MACHINE_VALUE_I64,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_IMMORTAL)
						: INVALID_VALUE_REF;
					if (loaded != INVALID_VALUE_REF) {
						add_payload_load_before(block_instructions,
							ctx.block, ctx.i, address, loaded,
							materialization.storage_id,
							ZEND_MIR_SCALAR_TYPE_I64, ctx.operand_offset);
						value = loaded;
					}
				}
			}
			if (value == INVALID_VALUE_REF
						|| !machine_value_has_result_representation(value)) {
				valid_ = false;
				operands_.push_back(INVALID_VALUE_REF);
			} else {
				operands_.push_back(value);
			}
			++ctx.operand_count;
		}
	}
}

uint32_t ZendIRAdaptor::Builder::append_boundary_operand(
		IRValueRef candidate, uint32_t segment_offset, uint32_t semantic_count,
		uint32_t &segment_count) {
		if (candidate == INVALID_VALUE_REF) {
			return UINT32_MAX;
		}
		const auto begin = operands_.begin() + segment_offset;
		const auto existing = std::find(
			begin, begin + semantic_count, candidate);
		if (existing != begin + semantic_count) {
			return UINT32_MAX;
		}
		const uint32_t index = segment_count++;
		operands_.push_back(candidate);
		return index;
}

void ZendIRAdaptor::Builder::reload_slot_authoritative_source_result(
		uint32_t i, const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record, uint32_t reload_block) {
	if (function_mode_ != FunctionMode::ZendEntry
			|| source_result_used[i] == 0
			|| record.opcode == ZEND_MIR_OPCODE_VALUE_INIT_ARRAY
			|| machine_value_has_result_representation(
				active_instruction_results()[i])) {
		return;
	}
	const IRValueRef canonical_result = source_binding_value_ref(
		instruction.source_result_binding);
	const uint32_t canonical_index =
		static_cast<uint32_t>(canonical_result);
	const zend_mir_storage_id storage_id =
		canonical_storage(canonical_result);
	if (canonical_result == INVALID_VALUE_REF
			|| !is_mir_value(canonical_index)
			|| !zend_mir_id_is_valid(storage_id)
			|| exact_type(canonical_result)
				== ZEND_MIR_SCALAR_TYPE_NULL) {
		return;
	}
	const IRValueRef address = frame_slot_address(storage_id);
	const bool source_strlen = instruction.has_value_operation
		&& record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
		&& instruction.value_operation.source_opcode == ZEND_STRLEN;
	const zend_mir_representation reload_representation =
		source_strlen
			? ZEND_MIR_REPRESENTATION_I64
			: representation(canonical_result);
	const zend_mir_scalar_type_mask reload_type =
		source_strlen
			? ZEND_MIR_SCALAR_TYPE_I64
			: exact_type(canonical_result);
	const zend_tpde_machine_value_kind reload_kind =
		source_strlen
			? ZEND_TPDE_MACHINE_VALUE_I64
			: machine_kind(canonical_result);
	const IRValueRef loaded = address != INVALID_VALUE_REF
		? add_derived_value(
			reload_representation, reload_type, storage_id, false, 0,
			reload_kind,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED,
			source_strlen
				? ZEND_MIR_REFCOUNT_IMMORTAL
				: refcount_state(canonical_result))
		: INVALID_VALUE_REF;
	if (loaded == INVALID_VALUE_REF) {
		valid_ = false;
		return;
	}
	const uint32_t load_operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.push_back(address);
	InstNode load{
		.kind = InstKind::ZvalPayloadLoad,
		.mir_instruction_index = i,
		.argument_index = UINT32_MAX,
		.result = loaded,
		.operand_offset = load_operand_offset,
		.operand_count = 1,
		.has_result = true,
		.storage_id = storage_id,
		.exact_type = reload_type};
	load.control_block = reload_block;
	add_node(block_instructions, reload_block, std::move(load));
	set_instruction_result(i, loaded);
}

void ZendIRAdaptor::Builder::materialize_boxed_result_for_helper(
		uint32_t i, uint32_t materialization_block, IRValueRef value) {
	if (source_result_materialized_for_helper[i] == 0) {
		return;
	}
	const zend_mir_storage_id storage = canonical_storage(value);
	if (value == INVALID_VALUE_REF
			|| machine_kind(value)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			|| !zend_mir_id_is_valid(storage)) {
		valid_ = false;
		return;
	}
	const uint32_t store_operand_offset =
		static_cast<uint32_t>(operands_.size());
	operands_.push_back(value);
	operands_.push_back(IRValueRef{FRAME_VALUE});
	InstNode store{
		.kind = InstKind::ZvalBoxedStore,
		.mir_instruction_index = i,
		.argument_index = UINT32_MAX,
		.result = INVALID_VALUE_REF,
		.operand_offset = store_operand_offset,
		.operand_count = 2,
		.has_result = false,
		.storage_id = storage};
	store.control_block = materialization_block;
	add_node(block_instructions, materialization_block,
		std::move(store));
}

void ZendIRAdaptor::Builder::lower_guarded_operation(InstContext &ctx) {
	GuardedSplit split;
	split.guarded_hot_block =
		guarded_hot_blocks[ctx.i];
	split.continuation_block =
		guarded_continuation_blocks[ctx.i];
	select_guarded_mutation_result(ctx, split);
	select_guarded_results(ctx, split);
	select_fast_operands(ctx, split);
	if (split.guarded_hot_block != UINT32_MAX) {
		add_typed_call_guard(ctx, split);
	}
	add_guarded_fast_node(ctx, split);
	select_cold_operands(ctx, split);
	add_guarded_cold_node(ctx, split);
	join_guarded_results(ctx, split);
	publish_assigned_pointer(ctx, split);
	reload_slot_authoritative_source_result(
		ctx.i, ctx.instruction, ctx.record, split.continuation_block);
}

void ZendIRAdaptor::Builder::select_guarded_mutation_result(
		InstContext &ctx, GuardedSplit &split) {
	split.frame_authoritative_mutation =
		ctx.instruction.has_value_operation
		&& std::binary_search(
			frame_authoritative_phi_storages.begin(),
			frame_authoritative_phi_storages.end(),
			ctx.instruction.value_operation.op1_storage_id);
	const IRValueRef canonical_mutation_result =
		mutation_results[ctx.i] != INVALID_VALUE_REF
			? mutation_results[ctx.i]
			: ctx.instruction.mutation_lazy_scalar
				? mutation_value_ref(ctx.instruction)
				: INVALID_VALUE_REF;
	const bool scalar_mutation_result =
		canonical_mutation_result != INVALID_VALUE_REF
		&& representation(canonical_mutation_result)
			== ZEND_MIR_REPRESENTATION_I64
		&& exact_type(canonical_mutation_result)
			== ZEND_MIR_SCALAR_TYPE_I64
		&& machine_kind(canonical_mutation_result)
			== ZEND_TPDE_MACHINE_VALUE_I64;
	const bool boxed_mutation_result =
		canonical_mutation_result != INVALID_VALUE_REF
		&& representation(canonical_mutation_result)
			== ZEND_MIR_REPRESENTATION_ZVAL
		&& zend_mir_id_is_valid(
			canonical_storage(canonical_mutation_result))
		&& canonical_storage(canonical_mutation_result)
			== ctx.instruction.value_operation.op1_storage_id;
	split.register_mutation_result =
		scalar_mutation_result || boxed_mutation_result;
	split.mutation_result =
		mutation_results[ctx.i] != INVALID_VALUE_REF
			? mutation_results[ctx.i]
			: canonical_mutation_result;
	if (split.register_mutation_result && boxed_mutation_result
			&& mutation_results[ctx.i]
				== INVALID_VALUE_REF) {
		const uint32_t mutation_ssa =
			ctx.instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one - 1;
		const int32_t mutation_index =
			zend_tpde_value_index(
				plan_,
				zend_mir_value_from_original_ssa(
					mutation_ssa));
		split.mutation_result = add_derived_value(
			ZEND_MIR_REPRESENTATION_ZVAL,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ctx.instruction.value_operation.op1_storage_id,
			false, 0,
			ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			ownership(canonical_mutation_result),
			refcount_state(canonical_mutation_result));
		if (mutation_index < 0
				|| static_cast<uint32_t>(mutation_index)
					>= active_value_overrides().size()
				|| mutation_ssa
					>= active_source_ssa_overrides().size()
				|| ctx.i >= active_instruction_results().size()) {
			valid_ = false;
		} else {
			active_value_overrides()[
				static_cast<uint32_t>(mutation_index)] =
				split.mutation_result;
			active_source_ssa_overrides()[mutation_ssa] =
				split.mutation_result;
			set_instruction_result(ctx.i, split.mutation_result);
		}
	} else if (split.register_mutation_result && scalar_mutation_result
			&& ctx.i < active_instruction_results().size()) {
		set_instruction_result(ctx.i, split.mutation_result);
	}
}

void ZendIRAdaptor::Builder::select_guarded_results(
		InstContext &ctx, GuardedSplit &split) {
	split.guarded_result =
		ctx.machine_result ? ctx.result
		: split.register_mutation_result
			? split.mutation_result : INVALID_VALUE_REF;
	/* A specialized member's operation whose failed guard
	 * deoptimizes: the cold block leaves the function and the
	 * continuation copies the fast result instead of merging. */
	split.deopt_exit =
		function_mode_ == FunctionMode::ZendEntry
		&& ctx.instruction.deopt_exit_resume_plus_one != 0;
	split.fast_result =
		split.guarded_result != INVALID_VALUE_REF
		? add_derived_value(
			representation(split.guarded_result),
			exact_type(split.guarded_result),
			ZEND_MIR_ID_INVALID, false, 0,
			machine_kind(split.guarded_result))
		: INVALID_VALUE_REF;
	split.cold_result =
		split.guarded_result != INVALID_VALUE_REF && !split.deopt_exit
		? add_derived_value(
			representation(split.guarded_result),
			exact_type(split.guarded_result),
			ZEND_MIR_ID_INVALID, false, 0,
			machine_kind(split.guarded_result))
		: INVALID_VALUE_REF;
}

void ZendIRAdaptor::Builder::select_fast_operands(
		InstContext &ctx, GuardedSplit &split) {
	split.fast_operand_offset = ctx.operand_offset;
	split.fast_operand_count = ctx.operand_count;
	split.fast_materialization_operand_index =
		ctx.materialization_operand_index;
	split.fast_materialization_count =
		ctx.materialization_count;
	split.fast_block = ctx.block;
	split.value_assign_source_operand =
		(ctx.record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
			|| ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN)
		&& ctx.operand_count != 0
		&& operands_[ctx.operand_offset]
			!= IRValueRef{FRAME_VALUE}
		&& !machine_reference(
			operands_[ctx.operand_offset], nullptr);
	const bool value_assign_register_source =
		split.value_assign_source_operand
		&& machine_value_is_register_authoritative(
			operands_[ctx.operand_offset])
		&& machine_value_has_register_definition(
			operands_[ctx.operand_offset]);
	if (split.value_assign_source_operand
			&& !value_assign_register_source) {
		/*
		 * copy_slot() reads canonical sources from the frame: no
		 * allocator-visible use for a value the selector never
		 * reads.
		 */
		++split.fast_operand_offset;
		--split.fast_operand_count;
		if (split.fast_materialization_operand_index
				!= UINT32_MAX) {
			--split.fast_materialization_operand_index;
		}
		if (ctx.machine_reference_operand_index != UINT32_MAX) {
			--ctx.machine_reference_operand_index;
		}
	}
	if (ctx.record.opcode
			== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
			&& ctx.static_slot_isset_machine_fast
			&& ctx.machine_result) {
		/*
		 * The exact scalar fast path consumes only its register
		 * operand; the frame and the materializations stay on the
		 * cold node for the helper.
		 */
		split.fast_operand_count =
			ctx.static_slot_isset_needs_value ? 2 : 1;
		split.fast_materialization_operand_index = UINT32_MAX;
		split.fast_materialization_count = 0;
	}
	if (ctx.register_bool_unary_operand != INVALID_VALUE_REF) {
		/*
		 * The register BOOL/BOOL_NOT consumes only its I1 input;
		 * the frame and the materializations stay on the cold node
		 * for the helper.
		 */
		split.fast_operand_count = 1;
		split.fast_materialization_operand_index = UINT32_MAX;
		split.fast_materialization_count = 0;
	}
}

void ZendIRAdaptor::Builder::add_typed_call_guard(
		InstContext &ctx, GuardedSplit &split) {
	/* Operands: the frame when boxed values are
	 * guarded, the statepoint materializations, then
	 * (value, expected type) pairs. */
	const uint32_t guard_operand_offset =
		static_cast<uint32_t>(operands_.size());
	const bool guarded_boxed_values =
		!ctx.typed_call_value_guards.empty();
	if (guarded_boxed_values) {
		operands_.push_back(IRValueRef{FRAME_VALUE});
	}
	for (uint32_t n = 0;
			n < ctx.materialization_count; ++n) {
		const uint32_t materialization_operand =
			ctx.materialization_operand_index + n;
		if (materialization_operand >= ctx.operand_count) {
			valid_ = false;
			operands_.push_back(INVALID_VALUE_REF);
		} else {
			operands_.push_back(
				operands_[ctx.operand_offset
					+ materialization_operand]);
		}
	}
	for (const auto &[guarded_value, expected_type] :
			ctx.typed_call_value_guards) {
		operands_.push_back(guarded_value);
		operands_.push_back(add_derived_value(
			ZEND_MIR_REPRESENTATION_I64,
			ZEND_MIR_SCALAR_TYPE_I64,
			ZEND_MIR_ID_INVALID, true, expected_type));
	}
	InstNode guard{
		.kind = InstKind::TypedCallGuard,
		.mir_instruction_index = ctx.i,
		.argument_index = ctx.guarded_cold_block,
		.result = INVALID_VALUE_REF,
		.operand_offset = guard_operand_offset,
		.operand_count = static_cast<uint32_t>(guarded_boxed_values)
			+ ctx.materialization_count
			+ static_cast<uint32_t>(
				ctx.typed_call_value_guards.size()) * 2,
		.has_result = false,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.inlined_user_body = false,
		.inlined_operand_index = UINT32_MAX,
		.materialization_operand_index = ctx.materialization_count == 0
			? UINT32_MAX
			: static_cast<uint32_t>(
				guarded_boxed_values),
		.materialization_count = ctx.materialization_count};
	guard.control_block = ctx.block;
	guard.continuation_block = split.guarded_hot_block;
	/* The guard reads all of its operands; liveness operands a
	 * generator resume block appends follow them. */
	guard.semantic_operand_count = guard.operand_count;
	add_node(block_instructions, ctx.block, std::move(guard));

	split.fast_operand_offset =
		static_cast<uint32_t>(operands_.size());
	const uint32_t fast_call_argument_count =
		frozen_typed_component_call(ctx.i)
			|| (ctx.instruction.direct_call->flags
				& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0
			? ctx.instruction.call_argument_count : 0;
	for (uint32_t n = 0;
			n < fast_call_argument_count; ++n) {
		operands_.push_back(
			operands_[ctx.operand_offset + n]);
	}
	for (IRValueRef owned_boxed :
			ctx.typed_call_owned_boxed_arguments) {
		operands_.push_back(owned_boxed);
	}
	split.fast_operand_count = fast_call_argument_count
		+ static_cast<uint32_t>(
			ctx.typed_call_owned_boxed_arguments.size());
	uint32_t fast_inlined_operand_index = UINT32_MAX;
	if (ctx.inlined_user_body.valid) {
		fast_inlined_operand_index = split.fast_operand_count;
		const uint32_t inlined_operand_count =
			ctx.inlined_user_body.operand_count();
		for (uint32_t n = 0;
				n < inlined_operand_count; ++n) {
			operands_.push_back(
				operands_[ctx.operand_offset
					+ ctx.inlined_operand_index + n]);
		}
		split.fast_operand_count += inlined_operand_count;
	}
	split.fast_materialization_operand_index = UINT32_MAX;
	split.fast_materialization_count = 0;
	split.fast_block = split.guarded_hot_block;
	ctx.inlined_operand_index =
		fast_inlined_operand_index;
}

void ZendIRAdaptor::Builder::add_guarded_fast_node(
		InstContext &ctx, GuardedSplit &split) {
	InstNode fast{
		.kind = InstKind::GuardedFast,
		.mir_instruction_index = ctx.i,
		.argument_index = ctx.guarded_cold_block,
		.result = split.fast_result,
		.operand_offset = split.fast_operand_offset,
		.operand_count = split.fast_operand_count,
		.has_result = split.fast_result != INVALID_VALUE_REF,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ctx.static_slot_isset_exact_type,
		.inlined_user_body = false,
		.inlined_operand_index = UINT32_MAX,
		.materialization_operand_index =
			split.fast_materialization_operand_index,
		.materialization_count = split.fast_materialization_count};
	fast.control_block = split.fast_block;
	fast.continuation_block = split.continuation_block;
	fast.machine_reference_operand_index =
		ctx.machine_reference_operand_index;
	fast.assign_op_right_operand_index =
		ctx.assign_op_right_operand_index;
	fast.assign_op_left_operand_index =
		ctx.assign_op_left_operand_index;
	fast.packed_append_value_operand_index =
		ctx.packed_append_value_operand_index;
	fast.property_write_value_operand_index =
		ctx.property_write_value_operand_index;
	fast.inlined_user_body = ctx.inlined_user_body.valid;
	fast.inlined_operand_index = ctx.inlined_operand_index;
	if (!ctx.inlined_user_body.checked_steps.empty()) {
		fast.inlined_checked_step_offset =
			static_cast<uint32_t>(inlined_checked_steps_.size());
		fast.inlined_checked_step_count = static_cast<uint32_t>(
			ctx.inlined_user_body.checked_steps.size());
		fast.inlined_checked_operand_count =
			ctx.inlined_user_body.operand_count();
		inlined_checked_steps_.insert(
			inlined_checked_steps_.end(),
			ctx.inlined_user_body.checked_steps.begin(),
			ctx.inlined_user_body.checked_steps.end());
	}
	fast.mutation_result = split.register_mutation_result;
	if (ctx.register_bool_unary_operand != INVALID_VALUE_REF) {
		fast.synthetic = true;
		fast.synthetic_record = ctx.record;
		fast.synthetic_record.opcode =
			ctx.instruction.value_operation.source_opcode
				== ZEND_BOOL_NOT
			? ZEND_MIR_OPCODE_I1_NOT
			: ZEND_MIR_OPCODE_COPY;
		fast.synthetic_record.representation =
			ZEND_MIR_REPRESENTATION_I1;
		fast.synthetic_record.effects = 0;
		fast.synthetic_record.reads = 0;
		fast.synthetic_record.writes = 0;
		fast.synthetic_record.barriers = 0;
		fast.synthetic_record.ownership_actions = 0;
	}
	add_node(block_instructions, split.fast_block, std::move(fast));
}

void ZendIRAdaptor::Builder::select_cold_operands(
		InstContext &ctx, GuardedSplit &split) {
	split.cold_operand_offset =
		static_cast<uint32_t>(operands_.size());
	split.cold_operand_count = ctx.operand_count;
	split.cold_materialization_operand_index =
		ctx.materialization_operand_index;
	if (ctx.record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		/*
		 * The split direct-call cold block enters and leaves
		 * through the runtime boundary and observes no fast-path
		 * arguments or inline-body values: as cold operands they
		 * would be false SSA uses and could let a chained call use
		 * a value before its continuation PHI defines it. The frame
		 * has a fixed assignment; the execution context is an
		 * ordinary SSA value that ENTER, the entry and LEAVE each
		 * use, so all three uses are explicit.
		 */
		operands_.push_back(IRValueRef{FRAME_VALUE});
		for (uint32_t use = 0; use < 3; ++use) {
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		}
		split.cold_operand_count =
			4 + ctx.materialization_count;
		split.cold_materialization_operand_index =
			ctx.materialization_count == 0
				? UINT32_MAX : 4;
		for (uint32_t n = 0;
				n < ctx.materialization_count; ++n) {
			operands_.push_back(
				operands_[ctx.operand_offset
					+ ctx.materialization_operand_index + n]);
		}
	} else {
		/*
		 * VALUE_ASSIGN's register boxed source is a fast-path
		 * operand: copy_slot() publishes it to the temporary before
		 * the cold edge, so the helper reads the frame and takes no
		 * second SSA use.
		 */
		uint32_t cold_operand_skip = 0;
		if (ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
			&& ctx.static_slot_isset_machine_fast
			&& ctx.machine_result) {
			cold_operand_skip =
				ctx.static_slot_isset_needs_value ? 1 : 0;
			while (cold_operand_skip + 1 < ctx.operand_count
					&& operands_[ctx.operand_offset
						+ cold_operand_skip]
						== IRValueRef{FRAME_VALUE}
					&& operands_[ctx.operand_offset
						+ cold_operand_skip + 1]
						== IRValueRef{FRAME_VALUE}) {
				++cold_operand_skip;
			}
		} else if (ctx.packed_append_value_operand_index != UINT32_MAX
				|| ctx.property_write_value_operand_index != UINT32_MAX) {
			cold_operand_skip = 1;
		} else if (ctx.register_bool_unary_operand != INVALID_VALUE_REF
				|| ctx.register_string_length_operand
					!= INVALID_VALUE_REF) {
			cold_operand_skip = 1;
		} else if (ctx.record.opcode
					== ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				&& ctx.operand_count != 0
				&& operands_[ctx.operand_offset]
					!= IRValueRef{FRAME_VALUE}
				&& machine_kind(operands_[ctx.operand_offset])
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
			cold_operand_skip = 1;
		} else if (split.value_assign_source_operand) {
			cold_operand_skip = 1;
		} else if (ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				&& ctx.materialization_operand_index
					!= UINT32_MAX) {
			while (cold_operand_skip
						< ctx.materialization_operand_index
					&& operands_[ctx.operand_offset
							+ cold_operand_skip]
						!= IRValueRef{FRAME_VALUE}) {
				++cold_operand_skip;
			}
		} else if (ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R) {
			while (cold_operand_skip < ctx.operand_count
					&& operands_[ctx.operand_offset
							+ cold_operand_skip]
						!= IRValueRef{FRAME_VALUE}) {
				++cold_operand_skip;
			}
		} else if (ctx.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM) {
			while (cold_operand_skip < ctx.operand_count
					&& operands_[ctx.operand_offset
						+ cold_operand_skip]
						!= IRValueRef{FRAME_VALUE}) {
				++cold_operand_skip;
			}
		}
		for (uint32_t n = cold_operand_skip;
				n < ctx.operand_count; ++n) {
			operands_.push_back(
				operands_[ctx.operand_offset + n]);
		}
		split.cold_operand_count -= cold_operand_skip;
		if (split.cold_materialization_operand_index != UINT32_MAX) {
			split.cold_materialization_operand_index -=
				cold_operand_skip;
		}
	}
}

void ZendIRAdaptor::Builder::add_guarded_cold_node(
		InstContext &ctx, GuardedSplit &split) {
	const uint32_t cold_semantic_operand_count =
		split.cold_materialization_operand_index == UINT32_MAX
			? split.cold_operand_count
			: split.cold_materialization_operand_index;
	uint32_t cold_boxed_op1_boundary_operand_index =
		append_boundary_operand(ctx.boxed_op1_boundary,
			split.cold_operand_offset, cold_semantic_operand_count,
			split.cold_operand_count);
	uint32_t cold_boxed_op2_boundary_operand_index = UINT32_MAX;
	if (ctx.boxed_op2_boundary
			== (cold_boxed_op1_boundary_operand_index == UINT32_MAX
				? INVALID_VALUE_REF
				: operands_[split.cold_operand_offset
					+ cold_boxed_op1_boundary_operand_index])) {
		cold_boxed_op2_boundary_operand_index =
			cold_boxed_op1_boundary_operand_index;
	} else {
		cold_boxed_op2_boundary_operand_index =
			append_boundary_operand(ctx.boxed_op2_boundary,
				split.cold_operand_offset, cold_semantic_operand_count,
				split.cold_operand_count);
	}
	InstNode cold{
		.kind = InstKind::GuardedCold,
		.mir_instruction_index = ctx.i,
		.argument_index = ctx.guarded_cold_block,
		.result = split.cold_result,
		.operand_offset = split.cold_operand_offset,
		.operand_count = split.cold_operand_count,
		.has_result = split.cold_result != INVALID_VALUE_REF,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.inlined_user_body = false,
		.inlined_operand_index = UINT32_MAX,
		.materialization_operand_index =
			split.cold_materialization_operand_index,
		.materialization_count = ctx.materialization_count};
	cold.control_block = ctx.guarded_cold_block;
	cold.continuation_block = split.continuation_block;
	cold.assign_op_right_operand_index =
		ctx.assign_op_right_operand_index;
	cold.assign_op_left_operand_index =
		ctx.assign_op_left_operand_index;
	cold.mutation_result = split.register_mutation_result;
	add_node(block_instructions, ctx.guarded_cold_block,
		std::move(cold));
	nodes_.back().deopt_exit = split.deopt_exit;
	if (cold_boxed_op1_boundary_operand_index != UINT32_MAX
			|| cold_boxed_op2_boundary_operand_index != UINT32_MAX) {
		nodes_.back().semantic_operand_count =
			cold_semantic_operand_count;
	}
	nodes_.back().boxed_op1_boundary_operand_index =
		cold_boxed_op1_boundary_operand_index;
	nodes_.back().boxed_op2_boundary_operand_index =
		cold_boxed_op2_boundary_operand_index;
	if (split.cold_result != INVALID_VALUE_REF
			&& ctx.record.source_position_id
				< user_opcode_result_reload_sources_.size()) {
		user_opcode_result_reload_sources_[
			ctx.record.source_position_id] = 1;
	}
}

void ZendIRAdaptor::Builder::join_guarded_results(
		InstContext &ctx, GuardedSplit &split) {
	if (split.mutation_result != INVALID_VALUE_REF
			&& !split.register_mutation_result) {
		const zend_mir_storage_id mutation_storage =
			canonical_storage(split.mutation_result);
		const IRValueRef mutation_slot =
			frame_slot_address(mutation_storage);
		if (mutation_slot == INVALID_VALUE_REF
				|| mutation_storage
					!= ctx.instruction.value_operation
						.op1_storage_id) {
			valid_ = false;
		} else {
			const uint32_t reload_operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(mutation_slot);
			InstNode reload{
				.kind = InstKind::ZvalPayloadLoad,
				.mir_instruction_index = ctx.i,
				.argument_index = UINT32_MAX,
				.result = split.mutation_result,
				.operand_offset = reload_operand_offset,
				.operand_count = 1,
				.has_result = true,
				.storage_id = mutation_storage,
				.exact_type = exact_type(split.mutation_result)};
			reload.control_block = split.continuation_block;
			add_node(block_instructions, split.continuation_block,
				std::move(reload));
		}
	}
	if (split.guarded_result != INVALID_VALUE_REF && split.deopt_exit) {
		const uint32_t copy_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(split.fast_result);
		InstNode copy{
			.kind = InstKind::MIR,
			.mir_instruction_index = ctx.i,
			.argument_index = UINT32_MAX,
			.result = split.guarded_result,
			.operand_offset = copy_operand_offset,
			.operand_count = 1,
			.has_result = true};
		copy.synthetic = true;
		copy.synthetic_record = ctx.record;
		copy.synthetic_record.opcode = ZEND_MIR_OPCODE_COPY;
		copy.synthetic_record.effects = 0;
		copy.synthetic_record.reads = 0;
		copy.synthetic_record.writes = 0;
		copy.synthetic_record.barriers = 0;
		copy.synthetic_record.ownership_actions = 0;
		copy.control_block = split.continuation_block;
		add_node(block_instructions, split.continuation_block,
			std::move(copy));
	}
	if (split.guarded_result != INVALID_VALUE_REF && !split.deopt_exit) {
		if (split.fast_result == INVALID_VALUE_REF
				|| split.cold_result == INVALID_VALUE_REF
				|| static_cast<uint32_t>(split.guarded_result)
					>= phi_input_slices_.size()
				|| phi_values_[
					static_cast<uint32_t>(
						split.guarded_result)] != 0) {
			valid_ = false;
		}
		const uint32_t phi_input_offset =
			static_cast<uint32_t>(phi_inputs_.size());
		phi_inputs_.push_back(
			{split.fast_result, IRBlockRef{
				split.guarded_hot_block == UINT32_MAX
					? ctx.block : split.guarded_hot_block}});
		phi_inputs_.push_back(
			{split.cold_result, IRBlockRef{ctx.guarded_cold_block}});
		phi_input_slices_[
			static_cast<uint32_t>(split.guarded_result)] = {
				phi_input_offset, 2};
		phi_values_[
			static_cast<uint32_t>(split.guarded_result)] = 1;
		block_phis.push_back(
			{split.continuation_block, split.guarded_result});
	}
	materialize_boxed_result_for_helper(
		ctx.i, split.continuation_block, split.guarded_result);
	if (split.frame_authoritative_mutation
			&& split.register_mutation_result
			&& mutation_results[ctx.i] != INVALID_VALUE_REF
			&& machine_kind(split.guarded_result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		const uint32_t store_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(split.guarded_result);
		operands_.push_back(IRValueRef{FRAME_VALUE});
		InstNode store{
			.kind = InstKind::ZvalBoxedStore,
			.mir_instruction_index = ctx.i,
			.argument_index = UINT32_MAX,
			.result = INVALID_VALUE_REF,
			.operand_offset = store_operand_offset,
			.operand_count = 2,
			.has_result = false,
			.storage_id = ctx.instruction.value_operation.op1_storage_id};
		store.control_block = split.continuation_block;
		add_node(block_instructions, split.continuation_block,
			std::move(store));
	}
}

void ZendIRAdaptor::Builder::publish_assigned_pointer(
		InstContext &ctx, GuardedSplit &split) {
	if (function_mode_ == FunctionMode::ZendEntry
			&& ctx.record.opcode
				== ZEND_MIR_OPCODE_VALUE_ASSIGN
			&& ctx.instruction.has_value_operation) {
		/*
		 * A refcounted assignment first updates the canonical slot,
		 * with replacement cleanup and ownership. The assigned
		 * payload pointer is stable after that and becomes the TPDE
		 * value, published at the joined continuation so the inline
		 * copy and the cold helper define the same register value.
		 * This keeps literal arrays and strings out of frame
		 * reloads in following loops.
		 */
		const uint32_t result_ssa =
			ctx.instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one;
		const IRValueRef canonical =
			result_ssa != 0
				? value_ref(
					zend_mir_value_from_original_ssa(
						result_ssa - 1))
				: INVALID_VALUE_REF;
		const int32_t canonical_value_index =
			result_ssa != 0
				? zend_tpde_value_index(
					plan_,
					zend_mir_value_from_original_ssa(
						result_ssa - 1))
				: -1;
		const uint8_t pointer_kind =
			canonical != INVALID_VALUE_REF
				? machine_kind(canonical) : UINT8_MAX;
		uint64_t literal_length = 0;
		bool literal_truthy = false;
		const bool string_literal = canonical
				!= INVALID_VALUE_REF
			&& known_string_literal(canonical,
				&literal_length, &literal_truthy);
		const uint8_t literal_first_byte = string_literal
				&& literal_length == 1 && !literal_truthy
			? '0' : 0;
		const zend_mir_storage_id storage_id =
			ctx.instruction.value_operation.op1_storage_id;
		const uint32_t reference =
			zend_mir_id_is_valid(storage_id)
				? machine_reference_index(
					ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
					storage_id)
				: UINT32_MAX;
		if ((pointer_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| pointer_kind
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
				|| pointer_kind
					== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
				|| pointer_kind
					== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR)
				&& valid_value_index(canonical_value_index)
				&& reference != UINT32_MAX) {
			const IRValueRef address = add_derived_value(
				ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
				ZEND_MIR_SCALAR_TYPE_NONE,
				storage_id, false, 0, UINT8_MAX,
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_UNKNOWN,
				reference);
			const IRValueRef pointer =
				ctx.i < register_assignment_results.size()
						&& register_assignment_results[ctx.i]
							!= INVALID_VALUE_REF
						&& zend_tpde_machine_kind_is_pointer(machine_kind(
							register_assignment_results[ctx.i]))
					? register_assignment_results[ctx.i]
					: add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						ZEND_MIR_SCALAR_TYPE_NONE,
						storage_id, false, 0, pointer_kind,
						ownership(canonical),
						refcount_state(canonical), UINT32_MAX,
						string_literal, literal_first_byte,
						literal_length);
			const uint32_t load_operand_offset =
				static_cast<uint32_t>(
					operands_.size());
			operands_.push_back(address);
			InstNode load{
				.kind = InstKind::ZvalPayloadLoad,
				.mir_instruction_index = ctx.i,
				.argument_index = UINT32_MAX,
				.result = pointer,
				.operand_offset = load_operand_offset,
				.operand_count = 1,
				.has_result = true,
				.storage_id = storage_id,
				.exact_type = ZEND_MIR_SCALAR_TYPE_NONE};
			load.control_block =
				split.continuation_block;
			add_node(block_instructions,
				split.continuation_block,
				std::move(load));
			auto &value_overrides =
				active_value_overrides();
			auto &source_overrides =
				active_source_ssa_overrides();
			set_instruction_result(ctx.i, pointer);
			value_overrides[
				static_cast<uint32_t>(
					canonical_value_index)] =
					pointer;
			if (result_ssa != 0
					&& result_ssa - 1
						< source_overrides.size()) {
				source_overrides[result_ssa - 1] =
					pointer;
			}
		}
	}
}

void ZendIRAdaptor::Builder::lower_boxed_cond_branch(InstContext &ctx) {
	const uint32_t semantic_operand_count =
		ctx.materialization_operand_index == UINT32_MAX
			? ctx.operand_count
			: ctx.materialization_operand_index;
	add_node(block_instructions,
		static_cast<uint32_t>(ctx.block), InstNode{
			.kind = InstKind::BoxedCondGuard,
			.mir_instruction_index = ctx.i,
			.argument_index = ctx.boxed_cond_cold_block,
			.result = ctx.result,
			.operand_offset = ctx.operand_offset,
			.operand_count = ctx.operand_count,
			.has_result = false,
			.storage_id = ZEND_MIR_ID_INVALID,
			.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
			.inlined_user_body = false,
			.inlined_operand_index = UINT32_MAX,
			.materialization_operand_index = ctx.materialization_operand_index,
			.materialization_count = ctx.materialization_count});
	nodes_.back().control_block = ctx.block;
	nodes_.back().cold_phi_inputs =
		has_pending_phi_inputs(ctx.boxed_cond_cold_block);
	IRValueRef boxed_condition = source_binding_value_ref(
		ctx.instruction.source_op1_binding);
	if (boxed_condition == INVALID_VALUE_REF) {
		boxed_condition = source_operand_value_ref(
			ctx.instruction.value_operation.op1);
	}
	if (boxed_condition != INVALID_VALUE_REF) {
		nodes_.back().exact_type = exact_type(boxed_condition);
	}
	/* Inference may know the condition is a boolean, such as a
	 * comparison result. */
	const uint32_t condition_position =
		ctx.instruction.value_operation.source_position_id;
	if (nodes_.back().exact_type == ZEND_MIR_SCALAR_TYPE_NONE
			&& plan_->source_opcodes != nullptr
			&& condition_position < plan_->source_opcode_count
			&& plan_->source_opcodes[condition_position]
					.op1_known_type
				== ZEND_TPDE_KNOWN_BOOL) {
		nodes_.back().exact_type = ZEND_MIR_SCALAR_TYPE_I1;
	}
	const uint32_t cold_operand_offset =
		static_cast<uint32_t>(operands_.size());
		const uint32_t cold_semantic_operand_count =
			semantic_operand_count
				- (ctx.register_boxed_condition_operand
						!= INVALID_VALUE_REF
					|| ctx.register_string_condition_operand
						!= INVALID_VALUE_REF);
		for (uint32_t n = 0;
				n < cold_semantic_operand_count; ++n) {
			operands_.push_back(
				operands_[ctx.operand_offset + n]);
	}
	for (uint32_t n = 0;
			n < ctx.materialization_count; ++n) {
		operands_.push_back(
			operands_[ctx.operand_offset
				+ ctx.materialization_operand_index + n]);
	}
	const bool split_cold_branch =
		has_pending_phi_inputs(ctx.boxed_cond_cold_block);
	const IRValueRef cold_decision = split_cold_branch
		? add_derived_value(
			ZEND_MIR_REPRESENTATION_I1,
			ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_ID_INVALID, false, 0,
			ZEND_TPDE_MACHINE_VALUE_BOOL)
		: INVALID_VALUE_REF;
	add_node(block_instructions, ctx.boxed_cond_cold_block, InstNode{
		.kind = InstKind::BoxedCondCold,
		.mir_instruction_index = ctx.i,
		.argument_index = ctx.boxed_cond_cold_block,
		.result = cold_decision,
		.operand_offset = cold_operand_offset,
		.operand_count = cold_semantic_operand_count
			+ ctx.materialization_count,
		.has_result = split_cold_branch,
		.storage_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.inlined_user_body = false,
		.inlined_operand_index = UINT32_MAX,
		.materialization_operand_index = ctx.materialization_count == 0
			? UINT32_MAX : cold_semantic_operand_count,
		.materialization_count = ctx.materialization_count});
	nodes_.back().control_block = ctx.boxed_cond_cold_block;
	if (split_cold_branch) {
		emit_pending_phi_inputs(ctx.boxed_cond_cold_block, ctx.i);
		const uint32_t branch_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(cold_decision);
		add_node(block_instructions,
			ctx.boxed_cond_cold_block, InstNode{
				.kind = InstKind::BoxedCondColdBranch,
				.mir_instruction_index = ctx.i,
				.argument_index = ctx.boxed_cond_cold_block,
				.result = INVALID_VALUE_REF,
				.operand_offset = branch_operand_offset,
				.operand_count = 1,
				.has_result = false});
		nodes_.back().control_block =
			ctx.boxed_cond_cold_block;
	}
}

void ZendIRAdaptor::Builder::verify_emission() {
	if (std::ranges::any_of(pending_phi_inputs,
			[](const PendingPhiInput &pending) {
				return !pending.emitted;
			})) {
		valid_ = false;
	}
	if (std::find(generator_resume_emitted.begin(),
			generator_resume_emitted.end(), 0)
			!= generator_resume_emitted.end()) {
		valid_ = false;
	}
}

void ZendIRAdaptor::Builder::index_landings() {
	freeze_deopt_landing_values(
		block_instructions, block_phis, tpde_block_count);
	for (uint32_t resume = 0; resume < deopt_resume_landed_.size();
			++resume) {
		if (deopt_resume_landed_[resume] == 0) {
			continue;
		}
		const uint32_t instruction =
			plan_->deopt_resume_instructions[resume];
		if (instruction >= deopt_resume_by_instruction_.size()) {
			deopt_resume_by_instruction_.resize(
				static_cast<size_t>(instruction) + 1, UINT32_MAX);
		}
		if (deopt_resume_by_instruction_[instruction] == UINT32_MAX) {
			deopt_resume_by_instruction_[instruction] = resume;
		}
	}
	for (const InstNode &load : nodes_) {
		if (load.kind != InstKind::ZvalPayloadLoad || !load.has_result
				|| !zend_mir_id_is_valid(load.storage_id)) {
			continue;
		}
		const uint32_t index = static_cast<uint32_t>(load.result);
		if (index >= slot_load_storage_.size()) {
			slot_load_storage_.resize(index + 1, ZEND_MIR_ID_INVALID);
		}
		slot_load_storage_[index] = load.storage_id;
	}
}

void ZendIRAdaptor::Builder::attach_generator_resume_values() {
	/*
	 * Reloaded generator values are definitions at the continuation entry,
	 * not uses: as GeneratorResume operands, a loop-carried PHI input would
	 * look dead right after the reload, because TPDE counts PHI-edge uses
	 * before the instructions of the predecessor. The values stay live to
	 * the end of that block without being semantic operands.
	 */
	/* The last item of each block: its terminal. */
	std::unordered_map<uint32_t, IRInstRef> block_terminals;
	if (plan_->generator_resume_count != 0) {
		for (const BlockItem<IRInstRef> &item : block_instructions) {
			block_terminals.insert_or_assign(item.block, item.value);
		}
	}
	for (const BlockItem<IRInstRef> &resume_item : block_instructions) {
		const InstNode &resume_node =
			nodes_[static_cast<uint32_t>(resume_item.value)];
		if (resume_node.kind != InstKind::GeneratorResume
				|| resume_node.generator_resume_value_count == 0) {
			continue;
		}
		const auto terminal = block_terminals.find(resume_item.block);
		if (terminal == block_terminals.end()) {
			valid_ = false;
			continue;
		}
		InstNode &terminal_node =
			nodes_[static_cast<uint32_t>(terminal->second)];
		const uint32_t old_offset = terminal_node.operand_offset;
		const uint32_t old_count = terminal_node.operand_count;
		const uint32_t old_liveness_operand_index =
			terminal_node.materialization_operand_index == UINT32_MAX
				? old_count
				: terminal_node.materialization_operand_index;
		const uint32_t new_offset =
			static_cast<uint32_t>(operands_.size());
		for (uint32_t operand = 0; operand < old_count; ++operand) {
			operands_.push_back(operands_[old_offset + operand]);
		}
		const uint32_t liveness_operand_offset =
			new_offset + old_liveness_operand_index;
		if (&terminal_node != &resume_node
				&& std::find(
					operands_.begin() + liveness_operand_offset,
					operands_.end(),
					IRValueRef{EXECUTION_CONTEXT_ARGUMENT})
					== operands_.end()) {
			/* The terminal reload resolves the resumed frame through the
			 * execution context, so keep that argument assigned even when
			 * no semantic instruction in the block otherwise consumes it. */
			operands_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		}
		const auto resume_value_span = std::span<const IRValueRef>{
			generator_resume_values_}.subspan(
				resume_node.generator_resume_value_offset,
				resume_node.generator_resume_value_count);
		const std::vector<IRValueRef> resume_values{
			resume_value_span.begin(), resume_value_span.end()};
		/* The terminal's statepoint materializations consume their own
		 * operands; the reload needs a use of its own. */
		const uint32_t resume_operand_offset = liveness_operand_offset
			+ (terminal_node.materialization_operand_index == UINT32_MAX
				? 0
				: static_cast<uint32_t>(
					materializations(terminal->second).size()));
		for (const IRValueRef value : resume_values) {
			if (std::find(
					operands_.begin() + resume_operand_offset,
					operands_.end(), value) == operands_.end()) {
				operands_.push_back(value);
			}
		}
		if (terminal_node.materialization_operand_index == UINT32_MAX) {
			terminal_node.materialization_operand_index = old_count;
		}
		terminal_node.operand_offset = new_offset;
		terminal_node.operand_count =
			static_cast<uint32_t>(operands_.size()) - new_offset;
		if (&terminal_node != &resume_node) {
			std::vector<IRValueRef> terminal_resume_values;
			if (terminal_node.generator_resume_value_count != 0) {
				const auto existing = std::span<const IRValueRef>{
					generator_resume_values_}.subspan(
						terminal_node.generator_resume_value_offset,
						terminal_node.generator_resume_value_count);
				terminal_resume_values.assign(
					existing.begin(), existing.end());
			}
			for (const IRValueRef value : resume_values) {
				if (std::find(terminal_resume_values.begin(),
						terminal_resume_values.end(), value)
						== terminal_resume_values.end()) {
					terminal_resume_values.push_back(value);
				}
			}
			terminal_node.generator_resume_value_offset =
				static_cast<uint32_t>(generator_resume_values_.size());
			terminal_node.generator_resume_value_count =
				static_cast<uint32_t>(terminal_resume_values.size());
			generator_resume_values_.insert(
				generator_resume_values_.end(),
				terminal_resume_values.begin(),
				terminal_resume_values.end());
		}
	}
}

void ZendIRAdaptor::Builder::emit_trailing_source_landings() {
	if (source_landings && plan_->source_block_ends != nullptr) {
		for (uint32_t source_block = 0;
				source_block < source_block_next.size(); ++source_block) {
			uint32_t &next_source = source_block_next[source_block];
			if (next_source == UINT32_MAX) {
				continue;
			}
			const uint32_t source_end =
				plan_->source_block_ends[source_block];
			while (next_source < source_end) {
				const uint32_t block =
					source_landing_blocks[next_source];
				if (block != UINT32_MAX) {
					emit_source_landing(block, next_source, UINT32_MAX);
				}
				++next_source;
			}
		}
	}
}

void ZendIRAdaptor::Builder::prune_unused_argument_loads() {
	/*
	 * A source argument may have an exact inferred type yet stay boxed-only
	 * (a by-reference parameter): loading and guarding its slot as a scalar
	 * would inspect the reference wrapper. Argument materialization
	 * therefore follows the final TPDE operand and PHI graph. The guard and
	 * payload-load nodes removed below use the frame and a slot address, so
	 * the answers are taken once.
	 */
	std::vector<uint8_t> operand_values_used(
		MIR_VALUE_BASE + plan_->value_count, 0);
	for (const BlockItem<IRInstRef> &item : block_instructions) {
		const InstNode &node = nodes_[static_cast<uint32_t>(item.value)];
		for (uint32_t operand = 0; operand < node.operand_count; ++operand) {
			const uint32_t raw = static_cast<uint32_t>(
				operands_[node.operand_offset + operand]);
			if (raw < operand_values_used.size()) {
				operand_values_used[raw] = 1;
			}
		}
	}
	for (const PhiInput &input : phi_inputs_) {
		const uint32_t raw = static_cast<uint32_t>(input.value);
		if (raw < operand_values_used.size()) {
			operand_values_used[raw] = 1;
		}
	}
	auto argument_machine_value_used = [&](uint32_t argument_index) {
		if (plan_->argument_value_indices == nullptr
				|| argument_index >= plan_->argument_count
				|| plan_->argument_value_indices[argument_index] < 0) {
			return false;
		}
		const uint32_t value_index = static_cast<uint32_t>(
			plan_->argument_value_indices[argument_index]);
		return (value_index < plan_->value_count
				&& operand_values_used[MIR_VALUE_BASE + value_index] != 0)
			|| machine_value_has_frozen_use(value_index);
	};
	std::erase_if(argument_guards_,
		[&](const ArgumentGuard &guard) {
			return !argument_machine_value_used(
				guard.argument_index);
		});
	block_instructions.erase(
		std::remove_if(block_instructions.begin(), block_instructions.end(),
			[&](const BlockItem<IRInstRef> &item) {
				if (item.block != static_cast<uint32_t>(entry)) {
					return false;
				}
				const InstNode &node =
					nodes_[static_cast<uint32_t>(item.value)];
				if (node.mir_instruction_index != UINT32_MAX) {
					return false;
				}
				switch (node.kind) {
					case InstKind::ZvalGuardArguments:
						return argument_guards_.empty();
					case InstKind::ZvalPayloadLoad:
						return node.argument_index != UINT32_MAX
							&& !argument_machine_value_used(
								node.argument_index);
					default:
						return false;
				}
			}),
		block_instructions.end());
}

void ZendIRAdaptor::Builder::resolve_deferred_typed_phi_inputs() {
	for (const auto &[input, value_id] : deferred_typed_phi_inputs) {
		const IRValueRef value = value_ref(value_id);
		if (value == INVALID_VALUE_REF
				|| machine_kind(value)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| !machine_value_has_register_definition(value)) {
			valid_ = false;
			continue;
		}
		phi_inputs_[input].value = value;
	}
}

void ZendIRAdaptor::Builder::finalize_graph() {
	/*
	 * The adaptor lives through the whole backend compilation: release the
	 * growth slack before handing the graph to TPDE.
	 */
	operands_.shrink_to_fit();
	for (InstNode &node : nodes_) {
		const auto all_operands =
			std::span<const IRValueRef>{operands_}.subspan(
				node.operand_offset, node.operand_count);
		const uint32_t semantic_operand_count =
			node.semantic_operand_count != UINT32_MAX
				? node.semantic_operand_count
				: node.materialization_operand_index == UINT32_MAX
					? node.operand_count
					: node.materialization_operand_index;
		node.operands = all_operands.first(semantic_operand_count);
		node.liveness_operands = all_operands;
	}
	flatten_block_items(tpde_block_count, block_instructions,
		instruction_slices_, instructions_);
	block_cold_.assign(tpde_block_count, 0);
	for (uint32_t block = 0; block < tpde_block_count; ++block) {
		for (IRInstRef inst : block_insts(IRBlockRef{block})) {
			const InstKind kind = node(inst).kind;
			if (kind == InstKind::GuardedCold
					|| kind == InstKind::BoxedCondCold) {
				block_cold_[block] = 1;
				break;
			}
		}
	}
	flatten_block_items(tpde_block_count, block_phis,
		phi_slices_, phis_);
	block_instructions.clear();
	block_instructions.shrink_to_fit();
	block_phis.clear();
	block_phis.shrink_to_fit();
	nodes_.shrink_to_fit();
	fused_instructions_.shrink_to_fit();
	instructions_.shrink_to_fit();
	phis_.shrink_to_fit();
}

} // namespace zend::native::tpde
