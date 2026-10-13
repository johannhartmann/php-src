// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

bool checked_count(uint32_t count) {
	return count <= MAX_RECORDS;
}

uint32_t id_index_capacity(uint32_t count) {
	uint32_t capacity = 8;

	while (capacity < count * 2) {
		capacity <<= 1;
	}
	return capacity;
}

static uint32_t id_index_hash(uint32_t id) {
	id ^= id >> 16;
	id *= UINT32_C(0x7feb352d);
	id ^= id >> 15;
	id *= UINT32_C(0x846ca68b);
	return id ^ (id >> 16);
}

bool id_index_insert(
	zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id,
	uint32_t index) {
	if (!zend_mir_id_is_valid(id)) {
		return false;
	}
	uint32_t slot = id_index_hash(id) & (capacity - 1);

	for (uint32_t probe = 0; probe < capacity; ++probe) {
		if (entries[slot].id == ZEND_MIR_ID_INVALID) {
			entries[slot] = {id, index};
			return true;
		}
		if (entries[slot].id == id) {
			return false;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return false;
}

int32_t id_index_find(
	const zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id) {
	if (entries == nullptr || !zend_mir_id_is_valid(id)) {
		return -1;
	}
	uint32_t slot = id_index_hash(id) & (capacity - 1);
	for (uint32_t probe = 0; probe < capacity; ++probe) {
		if (entries[slot].id == id) {
			return static_cast<int32_t>(entries[slot].index);
		}
		if (entries[slot].id == ZEND_MIR_ID_INVALID) {
			return -1;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return -1;
}

static zend_tpde_id_index_entry *allocate_id_index(
	uint32_t count, uint32_t *capacity_out) {
	if (count == 0) {
		*capacity_out = 0;
		return nullptr;
	}
	const uint32_t capacity = id_index_capacity(count);
	auto *entries = static_cast<zend_tpde_id_index_entry *>(
		std::malloc(static_cast<size_t>(capacity)
			* sizeof(zend_tpde_id_index_entry)));
	if (entries == nullptr) {
		return nullptr;
	}
	for (uint32_t i = 0; i < capacity; ++i) {
		entries[i] = {ZEND_MIR_ID_INVALID, 0};
	}
	*capacity_out = capacity;
	return entries;
}

void require_runtime_helper(
	zend_tpde_plan *plan, zend_native_runtime_helper_id helper) {
	plan->required_runtime_helpers[helper / 64u] |=
		UINT64_C(1) << (helper % 64u);
}

void destroy_plan(zend_tpde_plan *plan) {
	destroy_deopt_landing_adaptor(plan);
	for (uint32_t index = 0;
			plan->instructions != nullptr && index < plan->instruction_count;
			++index) {
		std::free(
			plan->instructions[index].direct_call_argument_guard_types);
	}
	free_call_descriptors(plan);
	for (uint32_t index = 0;
			index < plan->source_multi_branch_case_count; ++index) {
		std::free(plan->source_multi_branch_cases[index].string_key);
	}
	std::free(plan->block_ids);
	std::free(plan->block_index);
	std::free(plan->block_successor_offsets);
	std::free(plan->block_successors);
	std::free(plan->block_predecessor_offsets);
	std::free(plan->block_predecessors);
	std::free(plan->values);
	std::free(plan->argument_value_indices);
	std::free(plan->argument_abi);
	std::free(plan->typed_component_call_eligible);
	std::free(plan->effect_closed_inline_eligible);
	std::free(plan->effect_closed_argument_used);
	destroy_machine_cfg(&plan->entry_machine_cfg);
	destroy_machine_cfg(&plan->typed_body_machine_cfg);
	std::free(plan->value_index);
	std::free(plan->instructions);
	std::free(plan->instruction_operands);
	std::free(plan->instruction_operand_transports);
	std::free(plan->instruction_index);
	std::free(plan->value_definition_instructions);
	std::free(plan->source_value_definition_instructions);
	std::free(plan->value_consumer_offsets);
	std::free(plan->value_consumers);
	std::free(plan->entry_value_required);
	std::free(plan->typed_body_value_required);
	std::free(plan->source_opcodes);
	std::free(plan->source_multi_branches);
	std::free(plan->source_multi_branch_cases);
	std::free(plan->source_opcode_block_indices);
	std::free(plan->source_opcode_is_data);
	std::free(plan->source_block_starts);
	std::free(plan->source_block_ends);
	std::free(plan->source_call_phases);
	std::free(plan->compiled_variables_used);
	std::free(plan->call_site_instruction_index);
	std::free(plan->call_sites);
	std::free(plan->call_target_index);
	std::free(plan->call_arguments);
	std::free(plan->call_argument_bindings);
	std::free(plan->user_binding_index);
	std::free(plan->internal_binding_index);
	std::free(plan->user_opcode_source_operations);
	std::free(plan->user_opcode_source_op1_targets);
	std::free(plan->user_opcode_source_op2_targets);
	std::free(plan->user_opcode_source_extended_targets);
	std::free(plan->generator_resume_targets);
	std::free(plan->generator_resume_landings);
	std::free(plan->generator_resume_exception_blocks);
	std::free(plan->generator_resume_live_values);
	std::free(plan->deopt_resume_targets);
	std::free(plan->deopt_resume_instructions);
	std::free(plan->operand_observed_storages);
	std::free(plan->materializations);
	std::free(plan->machine_references);
	std::free(plan->entry_undef_temporary_indices);
	std::free(plan->entry_undef_temporary_fused_only);
	std::memset(plan, 0, sizeof(*plan));
}

/*
 * The numeric type of a source operand when a literal or Zend's type
 * inference fixes it: IS_LONG or IS_DOUBLE, otherwise IS_UNDEF.
 */
static uint8_t source_operand_known_type(
		const zend_op_array *op_array, const zend_ssa *ssa,
		uint32_t opline_index, uint8_t operand_type, znode_op operand,
		int ssa_use) {
	if (operand_type == IS_CONST) {
		const zval *literal = RT_CONSTANT(
			&op_array->opcodes[opline_index], operand);
		return Z_TYPE_P(literal) == IS_LONG || Z_TYPE_P(literal) == IS_DOUBLE
			? Z_TYPE_P(literal) : IS_UNDEF;
	}
	if ((operand_type & (IS_CV | IS_TMP_VAR | IS_VAR)) == 0
			|| ssa == nullptr || ssa->var_info == nullptr
			|| ssa_use < 0 || ssa_use >= ssa->vars_count) {
		return IS_UNDEF;
	}
	switch (ssa->var_info[ssa_use].type
			& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF | MAY_BE_INDIRECT)) {
		case MAY_BE_LONG:
			return IS_LONG;
		case MAY_BE_DOUBLE:
			return IS_DOUBLE;
		case MAY_BE_LONG | MAY_BE_DOUBLE:
			return ZEND_TPDE_KNOWN_NUMBER;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_FALSE | MAY_BE_TRUE:
			return ZEND_TPDE_KNOWN_BOOL;
		default:
			return IS_UNDEF;
	}
}

static uint32_t source_operand_may_be(
		const zend_op_array *op_array, const zend_ssa *ssa,
		uint32_t opline_index, uint8_t operand_type, znode_op operand,
		int ssa_use) {
	if (operand_type == IS_CONST) {
		const zval *literal = RT_CONSTANT(
			&op_array->opcodes[opline_index], operand);
		return 1u << Z_TYPE_P(literal);
	}
	if ((operand_type & (IS_CV | IS_TMP_VAR | IS_VAR)) == 0
			|| ssa == nullptr || ssa->var_info == nullptr
			|| ssa_use < 0 || ssa_use >= ssa->vars_count) {
		return UINT32_MAX;
	}
	return ssa->var_info[ssa_use].type;
}

/*
 * The receive facts of the native call entry, as
 * zend_native_call_fast_receive_prepare() derives the runtime's inline
 * receive: false when that receive does not apply.
 */
[[nodiscard]]
static bool try_freeze_fast_call_receive(
	const zend_op_array *op_array, zend_tpde_plan *plan) {
	const uint32_t scalar_types = (UINT32_C(1) << (IS_RESOURCE + 1)) - 1;

	if (op_array->num_args > ZEND_NATIVE_CALL_FAST_RECEIVE_MAX
			|| (op_array->num_args != 0 && op_array->arg_info == nullptr)) {
		return false;
	}
	plan->fast_call_min_arguments = 0;
	plan->fast_call_class_mask = 0;
	plan->fast_call_variadic =
		(op_array->fn_flags & ZEND_ACC_VARIADIC) != 0;
	if (plan->fast_call_variadic) {
		if (op_array->arg_info == nullptr
				|| op_array->num_args >= (uint32_t) op_array->last_var) {
			return false;
		}
		const zend_arg_info *variadic = &op_array->arg_info[op_array->num_args];
		if (ZEND_TYPE_IS_SET(variadic->type)
				|| ZEND_ARG_SEND_MODE(variadic) != 0) {
			return false;
		}
	}
	for (uint32_t index = 0; index < op_array->num_args; ++index) {
		const zend_op *opline = &op_array->opcodes[index];
		const zend_arg_info *info = &op_array->arg_info[index];
		const uint32_t mask = ZEND_TYPE_IS_SET(info->type)
			? static_cast<uint32_t>(ZEND_TYPE_PURE_MASK(info->type))
				& scalar_types
			: UINT32_MAX;

		plan->fast_call_type_masks[index] = mask;
		plan->fast_call_default_literals[index] = UINT32_MAX;
		if (ZEND_TYPE_IS_SET(info->type)
				&& (ZEND_TYPE_HAS_NAME(info->type)
					|| ZEND_TYPE_HAS_LIST(info->type))) {
			plan->fast_call_class_mask |= UINT32_C(1) << index;
		}
		if (ZEND_ARG_SEND_MODE(info) != 0) {
			/* A by-reference parameter is received by the general Do. */
			return false;
		}
		if (opline->opcode == ZEND_RECV) {
			plan->fast_call_min_arguments = index + 1;
			continue;
		}
		if (opline->opcode != ZEND_RECV_INIT
				|| opline->op1.num != index + 1
				|| opline->op2_type != IS_CONST) {
			return false;
		}
		const zval *value = RT_CONSTANT(opline, opline->op2);
		if (Z_TYPE_P(value) == IS_CONSTANT_AST || Z_REFCOUNTED_P(value)
				|| (mask & (UINT32_C(1) << Z_TYPE_P(value))) == 0) {
			return false;
		}
		plan->fast_call_default_literals[index] =
			static_cast<uint32_t>(value - op_array->literals);
	}
	return true;
}

/* What planning an instruction's part leaves to do. */
enum class plan_status : uint8_t {
	failed,
	finished,
	continues,
};

/* The member's entry facts: its kind, return type and fast-call receive. */
static void freeze_entry_facts(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	plan->source_generator = source_op_array == nullptr
		|| (source_op_array->fn_flags & ZEND_ACC_GENERATOR) != 0;
	plan->source_num_args =
		source_op_array != nullptr ? source_op_array->num_args : 0;
	plan->source_return_type_mask = source_op_array != nullptr
			&& (source_op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) != 0
			&& source_op_array->arg_info != nullptr
		? source_op_array->arg_info[-1].type.type_mask : 0;
	const uint32_t released_vars = source_op_array == nullptr ? 0
		: static_cast<uint32_t>(source_op_array->last_var);
	plan->fast_call_eligible = zend_tpde_linux_inline_forms
		&& source_op_array != nullptr
		&& source_op_array->function_name != nullptr
		&& (source_op_array->fn_flags & ZEND_ACC_GENERATOR) == 0
		&& released_vars <= 64
		&& source_op_array->num_args <= (uint32_t) source_op_array->last_var
		&& try_freeze_fast_call_receive(source_op_array, plan);
	plan->fast_call_counted_cvs = ~UINT64_C(0);
	if (plan->fast_call_eligible && source_ssa != nullptr
			&& source_ssa->vars != nullptr
			&& source_ssa->var_info != nullptr) {
		uint64_t counted = 0;
		for (int index = 0; index < source_ssa->vars_count; ++index) {
			const int variable = source_ssa->vars[index].var;
			if (variable < 0
					|| static_cast<uint32_t>(variable) >= released_vars) {
				continue;
			}
			const uint32_t type = source_ssa->var_info[index].type;
			if (type == 0 || (type & (MAY_BE_STRING | MAY_BE_ARRAY
					| MAY_BE_OBJECT | MAY_BE_RESOURCE | MAY_BE_REF)) != 0) {
				counted |= UINT64_C(1) << variable;
			}
		}
		/* A parameter the tier-2 entry guards holds any type when the
		 * guard fails: the tier-1 code then runs the frame, which the
		 * fast-call leave releases. */
		for (uint32_t parameter = 0;
				parameter < ZEND_NATIVE_TIER2_ARG_GUARDS
					&& parameter < released_vars; ++parameter) {
			if (plan->tier2_arg_guards[parameter]
					!= ZEND_NATIVE_ARG_GUARD_NONE) {
				counted |= UINT64_C(1) << parameter;
			}
		}
		plan->fast_call_counted_cvs = counted;
	}
	plan->source_literals =
		source_op_array != nullptr ? source_op_array->literals : nullptr;
	plan->source_literal_count = source_op_array != nullptr
		? static_cast<uint32_t>(source_op_array->last_literal) : 0;
}

[[nodiscard]]
static bool read_mir_function(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	zend_native_diagnostic *const diag = builder.diag;
	if (!zend_mir_contract_is_compatible(view->contract_version)
			|| view->function_count(view->context) != 1) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"requires one verified compatible MIR function");
		return false;
	}
	if (!view->function_at(view->context, 0, &plan->function)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR function table is unreadable");
		return false;
	}
	return true;
}

/* The source position a jump byte_offset from source_position reaches, or
 * UINT32_MAX. */
static uint32_t relative_source_target(
	const zend_op_array *op_array, uint32_t source_position,
	zend_long byte_offset) {
	if (op_array == nullptr || source_position >= op_array->last) {
		return UINT32_MAX;
	}
	const uintptr_t first = reinterpret_cast<uintptr_t>(op_array->opcodes);
	const intptr_t signed_address =
		reinterpret_cast<intptr_t>(&op_array->opcodes[source_position])
		+ static_cast<intptr_t>(byte_offset);
	const uintptr_t address = static_cast<uintptr_t>(signed_address);
	const uintptr_t last =
		first + static_cast<uintptr_t>(op_array->last) * sizeof(zend_op);
	if (address < first || address >= last
			|| (address - first) % sizeof(zend_op) != 0) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>((address - first) / sizeof(zend_op));
}

/* The multiway branches of SWITCH_LONG, SWITCH_STRING and MATCH. */
[[nodiscard]]
static bool freeze_source_multi_branches(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	if (!plan_alloc(plan->source_multi_branches, plan->source_opcode_count,
			diag, "unable to freeze source multiway metadata")) {
		return false;
	}
	uint64_t branch_case_count = 0;
	for (uint32_t index = 0;
			index < plan->source_opcode_count; ++index) {
		const zend_op &source = source_op_array->opcodes[index];
		if ((source.opcode != ZEND_SWITCH_LONG
					&& source.opcode != ZEND_SWITCH_STRING
					&& source.opcode != ZEND_MATCH)
				|| source.op2_type != IS_CONST) {
			continue;
		}
		const zval *jump_table = RT_CONSTANT(&source, source.op2);
		if (Z_TYPE_P(jump_table) != IS_ARRAY) {
			continue;
		}
		branch_case_count +=
			zend_hash_num_elements(Z_ARRVAL_P(jump_table));
		if (branch_case_count > MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source multiway case table exceeds executable bound");
			return false;
		}
	}
	plan->source_multi_branch_case_count =
		static_cast<uint32_t>(branch_case_count);
	if (branch_case_count != 0) {
		if (!plan_alloc(plan->source_multi_branch_cases, branch_case_count,
				diag, "unable to freeze source multiway cases")) {
			return false;
		}
	}
	uint32_t case_offset = 0;
	for (uint32_t index = 0;
			index < plan->source_opcode_count; ++index) {
		const zend_op &source = source_op_array->opcodes[index];
		if ((source.opcode != ZEND_SWITCH_LONG
					&& source.opcode != ZEND_SWITCH_STRING
					&& source.opcode != ZEND_MATCH)
				|| source.op2_type != IS_CONST) {
			continue;
		}
		const zval *jump_table = RT_CONSTANT(&source, source.op2);
		if (Z_TYPE_P(jump_table) != IS_ARRAY) {
			continue;
		}
		zend_tpde_source_multi_branch &branch =
			plan->source_multi_branches[index];
		branch.case_offset = case_offset;
		branch.case_count =
			zend_hash_num_elements(Z_ARRVAL_P(jump_table));
		branch.constant_successor = UINT32_MAX;
		branch.source_opcode = source.opcode;
		branch.default_target = relative_source_target(
			source_op_array, index,
			static_cast<zend_long>(source.extended_value));
		branch.fallback_target =
			index + 1 < plan->source_opcode_count
				? index + 1 : UINT32_MAX;
		branch.valid = branch.default_target != UINT32_MAX
			&& (source.opcode == ZEND_MATCH
				|| branch.fallback_target != UINT32_MAX);
		zend_ulong numeric_key;
		zend_string *string_key;
		zval *jump_value;
		const zval *constant_operand =
			source.opcode == ZEND_MATCH && source.op1_type == IS_CONST
				? RT_CONSTANT(&source, source.op1) : nullptr;
		uint32_t branch_case_index = 0;
		ZEND_HASH_FOREACH_KEY_VAL(
				Z_ARRVAL_P(jump_table),
				numeric_key, string_key, jump_value) {
			zend_tpde_multi_branch_case &frozen_case =
				plan->source_multi_branch_cases[case_offset++];
			if (Z_TYPE_P(jump_value) != IS_LONG) {
				branch.valid = false;
				branch_case_index++;
				continue;
			}
			if (constant_operand != nullptr
					&& branch.constant_successor == UINT32_MAX
					&& ((string_key == nullptr
							&& Z_TYPE_P(constant_operand) == IS_LONG
							&& Z_LVAL_P(constant_operand)
								== static_cast<zend_long>(numeric_key))
						|| (string_key != nullptr
							&& Z_TYPE_P(constant_operand) == IS_STRING
							&& zend_string_equals(
								Z_STR_P(constant_operand), string_key)))) {
				branch.constant_successor = branch_case_index;
			}
			frozen_case.target = relative_source_target(
				source_op_array, index, Z_LVAL_P(jump_value));
			if (frozen_case.target == UINT32_MAX) {
				branch.valid = false;
			}
			if (string_key == nullptr) {
				frozen_case.integer_key =
					static_cast<int64_t>(numeric_key);
				branch_case_index++;
				continue;
			}
			if (ZSTR_LEN(string_key) > UINT32_MAX) {
				branch.valid = false;
				branch_case_index++;
				continue;
			}
			frozen_case.string_length =
				static_cast<uint32_t>(ZSTR_LEN(string_key));
			const size_t allocation_size =
				frozen_case.string_length == 0
					? 1 : frozen_case.string_length;
			frozen_case.string_key =
				static_cast<char *>(std::malloc(allocation_size));
			if (frozen_case.string_key == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
					"unable to freeze source multiway string key");
				return false;
			}
			if (frozen_case.string_length != 0) {
				std::memcpy(frozen_case.string_key,
					ZSTR_VAL(string_key), frozen_case.string_length);
			}
			branch_case_index++;
		} ZEND_HASH_FOREACH_END();
		if (constant_operand != nullptr
				&& branch.constant_successor == UINT32_MAX) {
			branch.constant_successor = branch.case_count;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_source_opcodes(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	plan->source_ssa_variable_count =
		source_ssa != nullptr && source_ssa->vars_count > 0
			? static_cast<uint32_t>(source_ssa->vars_count) : 0;
	plan->source_opcode_count =
		source_op_array != nullptr ? source_op_array->last : 0;
	plan->source_frame_variable_count =
		source_op_array != nullptr ? source_op_array->last_var : 0;
	plan->source_temporary_count =
		source_op_array != nullptr ? source_op_array->T : 0;
	if (plan->source_opcode_count != 0) {
		if (!plan_alloc(plan->source_opcodes, plan->source_opcode_count,
				diag, "unable to freeze source opcode metadata")) {
			return false;
		}
		for (uint32_t index = 0;
				index < plan->source_opcode_count; ++index) {
			const zend_op &source = source_op_array->opcodes[index];
			zend_tpde_source_opcode &frozen = plan->source_opcodes[index];
			frozen.opcode = source.opcode;
			frozen.op1_type = source.op1_type;
			frozen.op2_type = source.op2_type;
			frozen.result_type = source.result_type;
			frozen.op1_var =
				source.op1_type == IS_CV || source.op1_type == IS_VAR
					|| source.op1_type == IS_TMP_VAR
				? source.op1.var : UINT32_MAX;
			frozen.op2_var =
				source.op2_type == IS_CV || source.op2_type == IS_VAR
					|| source.op2_type == IS_TMP_VAR
				? source.op2.var : UINT32_MAX;
			frozen.result_var =
				source.result_type == IS_CV
					|| source.result_type == IS_VAR
					|| source.result_type == IS_TMP_VAR
				? source.result.var : UINT32_MAX;
			frozen.extended_value = source.extended_value;
			frozen.op1_known_type = source_operand_known_type(
				source_op_array, source_ssa, index, source.op1_type,
				source.op1,
				source_ssa != nullptr && source_ssa->ops != nullptr
					? source_ssa->ops[index].op1_use : -1);
			frozen.op2_known_type = source_operand_known_type(
				source_op_array, source_ssa, index, source.op2_type,
				source.op2,
				source_ssa != nullptr && source_ssa->ops != nullptr
					? source_ssa->ops[index].op2_use : -1);
			frozen.op1_may_be = source_operand_may_be(
				source_op_array, source_ssa, index, source.op1_type,
				source.op1,
				source_ssa != nullptr && source_ssa->ops != nullptr
					? source_ssa->ops[index].op1_use : -1);
			frozen.op2_may_be = source_operand_may_be(
				source_op_array, source_ssa, index, source.op2_type,
				source.op2,
				source_ssa != nullptr && source_ssa->ops != nullptr
					? source_ssa->ops[index].op2_use : -1);
		}
		if (!freeze_source_multi_branches(builder)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_compiled_variable_uses(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	plan->compiled_variable_count =
		source_op_array != nullptr ? source_op_array->last_var : 0;
	if (plan->compiled_variable_count != 0) {
		plan->compiled_variables_used = static_cast<uint8_t *>(
			std::malloc(plan->compiled_variable_count));
		if (plan->compiled_variables_used == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze compiled-variable uses");
			return false;
		}
		std::memset(plan->compiled_variables_used, 1,
			plan->compiled_variable_count);
		if (source_ssa != nullptr && source_ssa->vars != nullptr
				&& source_ssa->vars_count >= 0) {
			/* A CV is unused when it has SSA variables and none of them is
			 * defined or used. */
			std::vector<uint8_t> found(plan->compiled_variable_count, 0);
			std::vector<uint8_t> used(plan->compiled_variable_count, 0);
			for (int index = 0; index < source_ssa->vars_count; ++index) {
				const zend_ssa_var &variable = source_ssa->vars[index];
				if (variable.var < 0
						|| static_cast<uint32_t>(variable.var)
							>= plan->compiled_variable_count) {
					continue;
				}
				const uint32_t variable_index =
					static_cast<uint32_t>(variable.var);
				found[variable_index] = 1;
				if (variable.definition >= 0
						|| variable.definition_phi != nullptr
						|| variable.use_chain >= 0
						|| variable.phi_use_chain != nullptr
						|| variable.sym_use_chain != nullptr) {
					used[variable_index] = 1;
				}
			}
			for (uint32_t variable_index = 0;
					variable_index < plan->compiled_variable_count;
					++variable_index) {
				plan->compiled_variables_used[variable_index] =
					found[variable_index] == 0 || used[variable_index] != 0;
			}
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_source_cfg(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	if (source_op_array != nullptr && source_ssa != nullptr
			&& source_ssa->cfg.blocks != nullptr
			&& source_ssa->cfg.map != nullptr
			&& source_ssa->cfg.blocks_count > 0) {
		plan->source_block_count =
			static_cast<uint32_t>(source_ssa->cfg.blocks_count);
		plan->source_opcode_block_indices = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_opcode_count)
				* sizeof(*plan->source_opcode_block_indices)));
		plan->source_opcode_is_data = static_cast<uint8_t *>(
			std::malloc(plan->source_opcode_count));
		plan->source_block_starts = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_block_count)
				* sizeof(*plan->source_block_starts)));
		plan->source_block_ends = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_block_count)
				* sizeof(*plan->source_block_ends)));
		if ((plan->source_opcode_count != 0
					&& (plan->source_opcode_block_indices == nullptr
						|| plan->source_opcode_is_data == nullptr))
				|| plan->source_block_starts == nullptr
				|| plan->source_block_ends == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze source CFG landings");
			return false;
		}
		for (uint32_t source = 0;
				source < plan->source_opcode_count; ++source) {
			const int source_block = source_ssa->cfg.map[source];
			plan->source_opcode_block_indices[source] =
				source_block >= 0
						&& static_cast<uint32_t>(source_block)
							< plan->source_block_count
					? static_cast<uint32_t>(source_block) : UINT32_MAX;
			plan->source_opcode_is_data[source] =
				source_op_array->opcodes[source].opcode == ZEND_OP_DATA;
		}
		for (uint32_t source_block = 0;
				source_block < plan->source_block_count; ++source_block) {
			const zend_basic_block &block =
				source_ssa->cfg.blocks[source_block];
			if ((block.flags & ZEND_BB_REACHABLE) == 0
					|| block.start > plan->source_opcode_count
					|| block.len
						> plan->source_opcode_count
							- block.start) {
				plan->source_block_starts[source_block] = UINT32_MAX;
				plan->source_block_ends[source_block] = UINT32_MAX;
				continue;
			}
			plan->source_block_starts[source_block] =
				block.start;
			plan->source_block_ends[source_block] =
				block.start + block.len;
		}
	}
	return true;
}

static void read_view_counts(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	plan->block_count = view->block_count(view->context);
	plan->value_count = view->value_count(view->context);
	plan->instruction_count = view->instruction_count(view->context);
	builder.calls = zend_mir_module_call_view_from_view(view);
	builder.value_model = zend_mir_module_value_view_from_view(view);
	const zend_mir_call_view *const calls = builder.calls;
	plan->call_site_count = calls != nullptr
		&& calls->call_site_count != nullptr
		? calls->call_site_count(calls->context) : 0;
	plan->call_target_count = calls != nullptr
		&& calls->call_target_count != nullptr
		? calls->call_target_count(calls->context) : 0;
	plan->call_argument_count = calls != nullptr
		&& calls->call_argument_count != nullptr
		? calls->call_argument_count(calls->context) : 0;
	builder.constant_count = view->constant_count(view->context);
	builder.frame_slot_count = view->frame_slot_count(view->context);
}

[[nodiscard]]
static bool freeze_user_opcode_operations(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	if (source_op_array == nullptr) {
		return true;
	}
	for (uint32_t index = 0; index < source_op_array->last; ++index) {
		if (zend_get_user_opcode_handler(
				source_op_array->opcodes[index].opcode) != nullptr) {
			plan->user_opcode_callbacks = true;
			plan->user_opcode_source_operation_count =
				source_op_array->last;
			plan->user_opcode_source_operations =
				static_cast<zend_mir_executable_value_ref *>(
					std::calloc(source_op_array->last,
						sizeof(*plan->user_opcode_source_operations)));
			plan->user_opcode_source_op1_targets =
				static_cast<uint32_t *>(std::malloc(
					static_cast<size_t>(source_op_array->last)
						* sizeof(uint32_t)));
			plan->user_opcode_source_op2_targets =
				static_cast<uint32_t *>(std::malloc(
					static_cast<size_t>(source_op_array->last)
						* sizeof(uint32_t)));
			plan->user_opcode_source_extended_targets =
				static_cast<uint32_t *>(std::malloc(
					static_cast<size_t>(source_op_array->last)
						* sizeof(uint32_t)));
			if (source_op_array->last != 0
					&& (plan->user_opcode_source_operations == nullptr
						|| plan->user_opcode_source_op1_targets == nullptr
						|| plan->user_opcode_source_op2_targets == nullptr
						|| plan->user_opcode_source_extended_targets
							== nullptr)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
					"unable to allocate user-opcode source operations");
				return false;
			}
			for (uint32_t source = 0;
					source < source_op_array->last; ++source) {
				if (!user_opcode_source_operation(
						source_op_array, source,
						&plan->user_opcode_source_operations[source])) {
					char message[192];
					std::snprintf(message, sizeof(message),
						"user-opcode source operands are invalid at source %u"
						" (opcode %u, types %u/%u/%u)",
						source,
						source_op_array->opcodes[source].opcode,
						source_op_array->opcodes[source].op1_type,
						source_op_array->opcodes[source].op2_type,
						source_op_array->opcodes[source].result_type);
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						message);
					return false;
				}
				const zend_op &opline =
					source_op_array->opcodes[source];
				plan->user_opcode_source_op1_targets[source] =
					user_opcode_source_absolute_target(
						source_op_array, source, opline.op1);
				plan->user_opcode_source_op2_targets[source] =
					user_opcode_source_absolute_target(
						source_op_array, source, opline.op2);
				plan->user_opcode_source_extended_targets[source] =
					user_opcode_source_extended_target(
						source_op_array, source,
						opline.extended_value);
			}
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_USER_OPCODE_INVOKE);
			require_runtime_helper(plan,
				ZEND_NATIVE_HELPER_GENERATOR_USER_OPCODE_RETURN);
			for (uint32_t opcode = 0;
					opcode <= ZEND_VM_LAST_OPCODE; ++opcode) {
				zend_tpde_user_opcode_target target;
				if (!user_opcode_target(opcode, &target)) {
					continue;
				}
				plan->user_opcode_targets[
					plan->user_opcode_target_count++] = target;
				if (target.helper != ZEND_NATIVE_HELPER_COUNT) {
					require_runtime_helper(plan, target.helper);
				}
			}
			break;
		}
	}
	return true;
}

[[nodiscard]]
static bool check_source_frame_bounds(plan_builder &builder) {
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	if (source_op_array == nullptr) {
		return true;
	}
	const uint32_t observer_temporary_count =
		ZEND_OBSERVER_ENABLED ? 1 : 0;
	if (source_op_array->last_var < 0
			|| !checked_count(
				static_cast<uint32_t>(source_op_array->last_var))
			|| !checked_count(source_op_array->T)
			|| source_op_array->T < observer_temporary_count
			|| static_cast<uint64_t>(source_op_array->last_var)
					+ source_op_array->T
				> MAX_RECORDS) {
		zend_tpde_set_diagnostic(
			diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source frame storage is outside the executable bound");
		return false;
	}
	return true;
}

[[nodiscard]]
static bool check_record_counts(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const uint32_t constant_count = builder.constant_count;
	const uint32_t frame_slot_count = builder.frame_slot_count;
	zend_native_diagnostic *const diag = builder.diag;
	if (plan->block_count == 0 || !checked_count(plan->block_count)
			|| !checked_count(plan->value_count)
			|| !checked_count(plan->instruction_count)
			|| !checked_count(plan->call_site_count)
			|| !checked_count(plan->call_target_count)
			|| !checked_count(plan->call_argument_count)
			|| !checked_count(constant_count)
			|| !checked_count(frame_slot_count)) {
		char message[192];
		std::snprintf(message, sizeof(message),
			"MIR record count exceeds executable bound"
			" (blocks=%u values=%u instructions=%u sites=%u targets=%u"
			" arguments=%u constants=%u frame_slots=%u)",
			plan->block_count, plan->value_count, plan->instruction_count,
			plan->call_site_count, plan->call_target_count,
			plan->call_argument_count, constant_count, frame_slot_count);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			message);
		return false;
	}
	return true;
}

[[nodiscard]]
static bool allocate_plan_tables(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const uint32_t user_binding_count = builder.user_binding_count;
	const uint32_t internal_binding_count =
		builder.internal_binding_count;
	const uint32_t frame_argument_count = builder.frame_argument_count;
	zend_native_diagnostic *const diag = builder.diag;
	plan->block_ids = static_cast<zend_mir_block_id *>(
		std::calloc(plan->block_count, sizeof(*plan->block_ids)));
	plan->block_index = allocate_id_index(
		plan->block_count, &plan->block_index_capacity);
	plan->values = static_cast<zend_tpde_value *>(
		std::calloc(plan->value_count, sizeof(*plan->values)));
	plan->argument_count = frame_argument_count;
	plan->argument_value_indices = static_cast<int32_t *>(
		std::malloc(
			static_cast<size_t>(plan->argument_count)
				* sizeof(*plan->argument_value_indices)));
	plan->argument_abi = static_cast<zend_tpde_local_abi_type *>(
		std::calloc(plan->argument_count, sizeof(*plan->argument_abi)));
	plan->effect_closed_argument_used = static_cast<uint8_t *>(
		std::calloc(plan->argument_count, sizeof(uint8_t)));
	for (uint32_t i = 0;
			i < plan->argument_count && plan->argument_value_indices != nullptr;
			++i) {
		plan->argument_value_indices[i] = -1;
	}
	plan->value_index = allocate_id_index(
		plan->value_count, &plan->value_index_capacity);
	plan->instructions = static_cast<zend_tpde_instruction *>(
		std::calloc(plan->instruction_count, sizeof(*plan->instructions)));
	plan->instruction_index = allocate_id_index(
		plan->instruction_count, &plan->instruction_index_capacity);
	plan->call_site_instruction_index = allocate_id_index(
		plan->call_site_count, &plan->call_site_instruction_index_capacity);
	plan->call_sites = static_cast<zend_mir_call_site_ref *>(
		std::malloc(static_cast<size_t>(plan->call_site_count)
			* sizeof(*plan->call_sites)));
	plan->call_target_index = allocate_id_index(
		plan->call_target_count, &plan->call_target_index_capacity);
	plan->call_argument_bindings =
		static_cast<zend_tpde_source_value_binding *>(std::malloc(
			static_cast<size_t>(plan->call_argument_count)
				* sizeof(*plan->call_argument_bindings)));
	plan->call_arguments =
		static_cast<zend_mir_call_argument_ref *>(std::malloc(
			static_cast<size_t>(plan->call_argument_count)
				* sizeof(*plan->call_arguments)));
	plan->user_binding_index = allocate_id_index(
		user_binding_count, &plan->user_binding_index_capacity);
	plan->internal_binding_index = allocate_id_index(
		internal_binding_count, &plan->internal_binding_index_capacity);
	plan->direct_calls = static_cast<zend_native_direct_call_descriptor **>(
		std::calloc(plan->call_site_count, sizeof(*plan->direct_calls)));
	plan->direct_internal_calls =
		static_cast<zend_native_direct_internal_call_descriptor **>(
			std::calloc(plan->call_site_count,
				sizeof(*plan->direct_internal_calls)));
	plan->user_calls = static_cast<zend_native_user_call_descriptor **>(
		std::calloc(plan->call_site_count, sizeof(*plan->user_calls)));
	if (plan->block_ids == nullptr || plan->block_index == nullptr
			|| (plan->value_count != 0
				&& (plan->values == nullptr || plan->value_index == nullptr))
			|| (plan->argument_count != 0
				&& (plan->argument_value_indices == nullptr
					|| plan->argument_abi == nullptr
					|| plan->effect_closed_argument_used == nullptr))
			|| (plan->instruction_count != 0
				&& (plan->instructions == nullptr
					|| plan->instruction_index == nullptr))
			|| (plan->call_site_count != 0
				&& (plan->call_site_instruction_index == nullptr
					|| plan->call_sites == nullptr
					|| plan->direct_calls == nullptr
					|| plan->direct_internal_calls == nullptr
					|| plan->user_calls == nullptr))
			|| (plan->call_target_count != 0
				&& plan->call_target_index == nullptr)
			|| (plan->call_argument_count != 0
				&& (plan->call_arguments == nullptr
					|| plan->call_argument_bindings == nullptr))
			|| (user_binding_count != 0 && plan->user_binding_index == nullptr)
			|| (internal_binding_count != 0
				&& plan->internal_binding_index == nullptr)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the TPDE adaptor plan");
		return false;
	}
	return true;
}

[[nodiscard]]
static bool freeze_instruction_table(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	zend_native_diagnostic *const diag = builder.diag;
	uint64_t operands = 0;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record;
		if (!view->instruction_at(view->context, i, &record)
				|| !id_index_insert(plan->instruction_index,
					plan->instruction_index_capacity, record.id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable or contains duplicate IDs");
			return false;
		}
		const uint32_t count =
			view->instruction_operand_count(view->context, record.id);
		operands += count;
		if (operands > MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR operand count is outside the executable bound");
			return false;
		}
		plan->instructions[i].record = record;
		plan->instructions[i].operand_offset =
			static_cast<uint32_t>(operands - count);
		plan->instructions[i].operand_count = count;
		plan->instructions[i].component_target_index = UINT32_MAX;
		plan->instructions[i].component_body_function_index = UINT32_MAX;
		plan->instructions[i].exception_block_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].zval_store_storage_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].zval_store_direct_scalar = false;
		plan->instructions[i].zval_store_lazy_scalar = false;
		plan->instructions[i].transient_scalar_result = false;
		plan->instructions[i].transient_result_storage_id =
			ZEND_MIR_ID_INVALID;
		plan->instructions[i].mutation_storage_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].mutation_lazy_scalar = false;
		plan->instructions[i].runtime_helper = ZEND_NATIVE_HELPER_COUNT;
		plan->instructions[i].source_opline_index = UINT32_MAX;
		plan->instructions[i].dynamic_fetch_cv_index = UINT32_MAX;
		plan->instructions[i].dynamic_fetch_direct_long = false;
		plan->instructions[i].source_op1_binding = {-1, -1};
		plan->instructions[i].source_op2_binding = {-1, -1};
		plan->instructions[i].source_op2_definition_binding = {-1, -1};
		plan->instructions[i].source_op2_canonical_scalar_only = false;
		plan->instructions[i].source_result_binding = {-1, -1};
		plan->instructions[i].source_auxiliary_binding = {-1, -1};
	}
	plan->instruction_operand_count = static_cast<uint32_t>(operands);
	plan->instruction_operands = static_cast<zend_mir_value_id *>(
		std::malloc(static_cast<size_t>(plan->instruction_operand_count)
			* sizeof(*plan->instruction_operands)));
	if (plan->instruction_operand_count != 0
			&& plan->instruction_operands == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR instruction operands");
		return false;
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_tpde_instruction &instruction = plan->instructions[i];
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			zend_mir_value_id value = ZEND_MIR_ID_INVALID;
			if (!view->instruction_operand_at(view->context,
					instruction.record.id, operand, &value)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR instruction operand table is unreadable");
				return false;
			}
			plan->instruction_operands[
				instruction.operand_offset + operand] = value;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_call_tables(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_call_view *const calls = builder.calls;
	const zend_native_call_binding *const user_bindings =
		builder.user_bindings;
	const uint32_t user_binding_count = builder.user_binding_count;
	const zend_native_internal_call_binding *const internal_bindings =
		builder.internal_bindings;
	const uint32_t internal_binding_count =
		builder.internal_binding_count;
	zend_native_diagnostic *const diag = builder.diag;
	for (uint32_t i = 0; i < plan->call_site_count; ++i) {
		zend_mir_call_site_ref site;
		if (calls == nullptr
				|| !calls->call_site_at(calls->context, i, &site)
				|| zend_tpde_instruction_index(plan, site.instruction_id) < 0
				|| !id_index_insert(plan->call_site_instruction_index,
					plan->call_site_instruction_index_capacity,
					site.instruction_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-site table is unreadable, duplicated, or references an unknown instruction");
			return false;
		}
		plan->call_sites[i] = site;
	}
	for (uint32_t i = 0; i < plan->call_target_count; ++i) {
		zend_mir_call_target_ref target;
		if (calls == nullptr
				|| !calls->call_target_at(calls->context, i, &target)
				|| !id_index_insert(plan->call_target_index,
					plan->call_target_index_capacity, target.id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-target table is unreadable or contains duplicate IDs");
			return false;
		}
	}
	for (uint32_t i = 0; i < plan->call_argument_count; ++i) {
		zend_mir_call_argument_ref argument;
		plan->call_argument_bindings[i] = {-1, -1};
		if (calls == nullptr
				|| !calls->call_argument_at(calls->context, i, &argument)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-argument table is unreadable");
			return false;
		}
		plan->call_arguments[i] = argument;
	}
	for (uint32_t i = 0; i < user_binding_count; ++i) {
		if (user_bindings[i].entry_cell == nullptr
				|| !id_index_insert(plan->user_binding_index,
					plan->user_binding_index_capacity,
					user_bindings[i].target_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"direct user-call binding table is invalid or duplicated");
			return false;
		}
	}
	for (uint32_t i = 0; i < internal_binding_count; ++i) {
		if (internal_bindings[i].call_cell == nullptr
				|| !id_index_insert(plan->internal_binding_index,
					plan->internal_binding_index_capacity,
					internal_bindings[i].target_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"direct internal-call binding table is invalid or duplicated");
			return false;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_value_operations(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_value_view *const value_model = builder.value_model;
	zend_native_diagnostic *const diag = builder.diag;
	/*
	 * Value lowering always commits a canonical-location model; a module
	 * without one cannot be compiled.
	 */
	if (value_model == nullptr
			|| value_model->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| value_model->model_flags
				!= ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"executable value model lacks the machine-plan facts");
		return false;
	}
	const uint32_t operation_count =
		value_model->executable_operation_count(value_model->context);
	if (!checked_count(operation_count)
			|| operation_count > plan->instruction_count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"executable value operation count is outside the MIR bound");
		return false;
	}
	for (uint32_t i = 0; i < operation_count; ++i) {
		zend_mir_executable_value_ref operation{};
		int32_t instruction_index;
		if (!value_model->executable_operation_at(
				value_model->context, i, &operation)
				|| (instruction_index =
						zend_tpde_instruction_index(plan, operation.id)) < 0
				|| plan->instructions[instruction_index]
					.has_value_operation) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"executable value operation has an invalid instruction identity");
			return false;
		}
		plan->instructions[instruction_index].value_operation = operation;
		plan->instructions[instruction_index].has_value_operation = true;
	}
	return true;
}

[[nodiscard]]
static bool freeze_source_zval_returns(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	/*
	 * A source-zval RETURN without an executable value operation has the
	 * runtime ABI of an executable RETURN. Give it a value operation here,
	 * while the source and SSA views are available, so generated code never
	 * decodes a Zend opcode.
	 */
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_tpde_instruction &instruction = plan->instructions[i];
		const zend_mir_instruction_record &record = instruction.record;
		if (instruction.has_value_operation
				|| record.opcode != ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			continue;
		}
		if (source_op_array == nullptr
				|| record.source_position_id >= source_op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-zval return has no source descriptor");
			return false;
		}
		const zend_op *opline =
			&source_op_array->opcodes[record.source_position_id];
		if ((opline->opcode != ZEND_RETURN
					&& opline->opcode != ZEND_RETURN_BY_REF)
				|| !source_descriptor_operand(
					source_op_array, opline, opline->op1_type,
					opline->op1, &instruction.value_operation.op1)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->op2,
					&instruction.value_operation.op2)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->result,
					&instruction.value_operation.result)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->result,
					&instruction.value_operation.auxiliary)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-zval return operand is invalid");
			return false;
		}
		instruction.value_operation.id = record.id;
		instruction.value_operation.block_id = record.block_id;
		instruction.value_operation.opcode = record.opcode;
		instruction.value_operation.source_opcode = opline->opcode;
		instruction.value_operation.op1_storage_id =
			source_descriptor_storage(
				source_op_array, instruction.value_operation.op1);
		instruction.value_operation.op2_storage_id = ZEND_MIR_ID_INVALID;
		instruction.value_operation.result_storage_id = ZEND_MIR_ID_INVALID;
		instruction.value_operation.auxiliary_storage_id =
			ZEND_MIR_ID_INVALID;
		instruction.value_operation.extended_value = opline->extended_value;
		instruction.value_operation.source_position_id =
			record.source_position_id;
		instruction.value_operation.frame_state_id = record.frame_state_id;
		instruction.value_operation.effects = record.effects;
		instruction.value_operation.reads = record.reads;
		instruction.value_operation.writes = record.writes;
		instruction.value_operation.barriers = record.barriers;
		instruction.value_operation.ownership_actions =
			record.ownership_actions;
		if (source_ssa != nullptr && source_ssa->ops != nullptr
				&& source_ssa->ops[record.source_position_id].op1_use >= 0) {
			instruction.value_operation.op1.kind =
				ZEND_MIR_SOURCE_OPERAND_SSA;
			instruction.value_operation.op1.ssa_variable_id =
				static_cast<uint32_t>(
					source_ssa->ops[record.source_position_id].op1_use);
		}
		instruction.has_value_operation = true;
	}
	return true;
}

[[nodiscard]]
static bool freeze_block_graph(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	zend_native_diagnostic *const diag = builder.diag;
	for (uint32_t i = 0; i < plan->block_count; ++i) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, i, &block)
				|| block.function_id != plan->function.id
				|| !id_index_insert(plan->block_index,
					plan->block_index_capacity, block.id, i)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR block table is inconsistent");
			return false;
		}
		plan->block_ids[i] = block.id;
	}
	plan->block_successor_offsets = static_cast<uint32_t *>(
		std::calloc(static_cast<size_t>(plan->block_count) + 1,
			sizeof(*plan->block_successor_offsets)));
	plan->block_predecessor_offsets = static_cast<uint32_t *>(
		std::calloc(static_cast<size_t>(plan->block_count) + 1,
			sizeof(*plan->block_predecessor_offsets)));
	if (plan->block_successor_offsets == nullptr
			|| plan->block_predecessor_offsets == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR CFG offsets");
		return false;
	}
	for (uint32_t block = 0; block < plan->block_count; ++block) {
		const uint32_t successor_count = view->successor_count(
			view->context, plan->block_ids[block]);
		const uint32_t predecessor_count = view->predecessor_count(
			view->context, plan->block_ids[block]);
		if (successor_count
					> UINT32_MAX - plan->block_successor_count
				|| predecessor_count
					> UINT32_MAX - plan->block_predecessor_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR CFG edge count overflows the machine plan");
			return false;
		}
		plan->block_successor_count += successor_count;
		plan->block_predecessor_count += predecessor_count;
		plan->block_successor_offsets[block + 1] =
			plan->block_successor_count;
		plan->block_predecessor_offsets[block + 1] =
			plan->block_predecessor_count;
	}
	if (plan->block_successor_count != 0) {
		plan->block_successors = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->block_successor_count)
				* sizeof(*plan->block_successors)));
	}
	if (plan->block_predecessor_count != 0) {
		plan->block_predecessors = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->block_predecessor_count)
				* sizeof(*plan->block_predecessors)));
	}
	if ((plan->block_successor_count != 0
				&& plan->block_successors == nullptr)
			|| (plan->block_predecessor_count != 0
				&& plan->block_predecessors == nullptr)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR CFG edges");
		return false;
	}
	for (uint32_t block = 0; block < plan->block_count; ++block) {
		const uint32_t successor_begin =
			plan->block_successor_offsets[block];
		const uint32_t successor_count =
			plan->block_successor_offsets[block + 1] - successor_begin;
		for (uint32_t edge = 0; edge < successor_count; ++edge) {
			zend_mir_block_id target;
			const int32_t target_index =
				view->successor_at(view->context, plan->block_ids[block],
						edge, &target)
					? id_index_find(plan->block_index,
						plan->block_index_capacity, target)
					: -1;
			if (target_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR successor is outside the machine plan");
				return false;
			}
			plan->block_successors[successor_begin + edge] =
				static_cast<uint32_t>(target_index);
		}
		const uint32_t predecessor_begin =
			plan->block_predecessor_offsets[block];
		const uint32_t predecessor_count =
			plan->block_predecessor_offsets[block + 1]
				- predecessor_begin;
		for (uint32_t edge = 0; edge < predecessor_count; ++edge) {
			zend_mir_block_id predecessor;
			const int32_t predecessor_index =
				view->predecessor_at(view->context, plan->block_ids[block],
						edge, &predecessor)
					? id_index_find(plan->block_index,
						plan->block_index_capacity, predecessor)
					: -1;
			if (predecessor_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR predecessor is outside the machine plan");
				return false;
			}
			plan->block_predecessors[predecessor_begin + edge] =
				static_cast<uint32_t>(predecessor_index);
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_values(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	const zend_mir_value_view *const value_model = builder.value_model;
	zend_native_diagnostic *const diag = builder.diag;
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		zend_mir_value_record value;
		if (!view->value_at(view->context, i, &value)
				|| !id_index_insert(plan->value_index,
					plan->value_index_capacity, value.id, i)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR value table is unreadable or contains duplicate IDs");
			return false;
		}
		plan->values[i] = {
			.id = value.id,
			.representation = value.representation,
			.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
			.canonical_storage_id = ZEND_MIR_ID_INVALID,
			.ownership = value.ownership,
			.category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN,
			.refcount_state = ZEND_MIR_REFCOUNT_UNKNOWN,
			.argument_index = -1,
			.register_alias_value_index = -1,
			.machine_kind = ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			.location = ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT,
			.slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED,
			.canonical_alias_observable = false,
			.constant = false,
			.constant_bits = 0,
			.known_string_literal = false,
			.known_string_first_byte = 0,
			.known_string_length = 0,
		};
	}
	const uint32_t location_count =
		value_model->value_location_count(value_model->context);
	if (!checked_count(location_count)
			|| location_count > plan->value_count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"value-location table is outside the MIR value bound");
		return false;
	}
	for (uint32_t i = 0; i < location_count; ++i) {
		zend_mir_value_location_ref location{};
		int32_t value_index;
		if (!value_model->value_location_at(
				value_model->context, i, &location)
				|| !zend_mir_id_is_valid(location.storage_id)
				|| (location.category
							< ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						|| location.category
							> ZEND_MIR_VALUE_CATEGORY_UNKNOWN
						|| location.refcount_state
							< ZEND_MIR_REFCOUNT_IMMORTAL
						|| location.refcount_state
							> ZEND_MIR_REFCOUNT_UNKNOWN
						|| (location.alias_observable
							&& location.category
								!= ZEND_MIR_VALUE_REFERENCE_CELL))
				|| (value_index = zend_tpde_value_index(
						plan, location.value_id)) < 0
				|| zend_mir_id_is_valid(
					plan->values[value_index].canonical_storage_id)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"value-location table is unreadable, duplicated, or invalid");
			return false;
		}
		plan->values[value_index].canonical_storage_id =
			location.storage_id;
		plan->values[value_index].category = location.category;
		plan->values[value_index].refcount_state =
			location.refcount_state;
		plan->values[value_index].canonical_alias_observable =
			location.alias_observable;
		if (location.frame_argument_ordinal_plus_one != 0) {
			const uint32_t argument_index =
				location.frame_argument_ordinal_plus_one - 1;
			if (argument_index >= plan->argument_count
					|| argument_index > static_cast<uint32_t>(INT32_MAX)
					|| plan->argument_value_indices[argument_index] >= 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"value-location table has an invalid or duplicate frame argument");
				return false;
			}
			plan->argument_value_indices[argument_index] = value_index;
			plan->values[value_index].argument_index =
				static_cast<int32_t>(argument_index);
			plan->values[value_index].constant = false;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_constants(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	const uint32_t constant_count = builder.constant_count;
	zend_native_diagnostic *const diag = builder.diag;
	for (uint32_t i = 0; i < constant_count; ++i) {
		zend_mir_constant_record constant;
		if (!view->constant_at(view->context, i, &constant)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR constant table is unreadable");
			return false;
		}
		int32_t index = zend_tpde_value_index(plan, constant.value_id);
		if (index < 0) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR constant references an unknown value");
			return false;
		}
		plan->values[index].constant = true;
		switch (constant.kind) {
			case ZEND_MIR_CONSTANT_KIND_NULL_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_NULL;
				plan->values[index].constant_bits = 0;
				break;
			case ZEND_MIR_CONSTANT_KIND_FALSE_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I1;
				plan->values[index].constant_bits = 0;
				break;
			case ZEND_MIR_CONSTANT_KIND_TRUE_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I1;
				plan->values[index].constant_bits = 1;
				break;
			case ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I64;
				plan->values[index].constant_bits = constant.payload_bits;
				break;
			case ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_F64;
				plan->values[index].constant_bits = constant.payload_bits;
				break;
			default:
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
					"pointer and string constants are not executable");
				return false;
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_value_facts(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	const uint32_t value_fact_count = view->value_fact_count(view->context);
	if (!checked_count(value_fact_count)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR value-fact count is outside the executable bound");
		return false;
	}
	for (uint32_t i = 0; i < value_fact_count; ++i) {
		zend_mir_value_fact_ref fact;
		if (!view->value_fact_at(view->context, i, &fact)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR value-fact table is unreadable");
			return false;
		}
		int32_t index = zend_tpde_value_index(plan, fact.value_id);
		if (index >= 0 && zend_mir_scalar_type_is_exact(fact.exact_type)) {
			plan->values[index].exact_type = fact.exact_type;
		}
	}
	std::vector<uint8_t> register_definitions(plan->value_count, 0);
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record &record =
			plan->instructions[i].record;
		if (!zend_mir_id_is_valid(record.result_id)) {
			continue;
		}
		const int32_t value_index =
			zend_tpde_value_index(plan, record.result_id);
		if (value_index < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction result is absent from the value table");
			return false;
		}
		register_definitions[static_cast<uint32_t>(value_index)] = 1;
	}
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		zend_tpde_apply_machine_value_facts(
			&plan->values[i], register_definitions[i] != 0);
	}
	zend_tpde_refine_non_alias_scalar_values(
		plan, source_op_array, source_ssa, register_definitions);
	zend_tpde_refine_literal_assignment_values(
		plan, source_op_array, source_ssa);
	zend_tpde_refine_boxed_scalar_copies(plan);
	return true;
}

static void freeze_argument_abi(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	for (uint32_t argument = 0;
			argument < plan->argument_count; ++argument) {
		const zend_arg_info *arg_info =
			source_op_array != nullptr
				&& source_op_array->arg_info != nullptr
				&& argument < source_op_array->num_args
			? &source_op_array->arg_info[argument] : nullptr;
		const bool by_reference =
			arg_info != nullptr
			&& (ZEND_ARG_SEND_MODE(arg_info)
				& (ZEND_SEND_BY_REF | ZEND_SEND_PREFER_REF)) != 0;
		plan->argument_abi[argument] =
			zend_tpde_local_abi_from_declared_type(
				arg_info == nullptr ? nullptr : &arg_info->type,
				by_reference,
				ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED);
		/* An effect-closed clone has no receive: it needs every argument
		 * whose declared type the receive would check or coerce. */
		plan->effect_closed_argument_used[argument] = arg_info == nullptr
			? source_op_array == nullptr
				|| argument >= source_op_array->num_args
			: ZEND_TYPE_IS_SET(arg_info->type)
				&& (ZEND_TYPE_FULL_MASK(arg_info->type) & MAY_BE_ANY)
					!= MAY_BE_ANY;
		if (arg_info != nullptr && ZEND_TYPE_IS_SET(arg_info->type)
				&& plan->argument_abi[argument].machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& (ZEND_TYPE_FULL_MASK(arg_info->type) & MAY_BE_ANY)
					!= MAY_BE_ANY) {
			const uint32_t bit = argument < 32
				? UINT32_C(1) << argument : UINT32_C(0);
			plan->checked_boxed_argument_mask |= argument < 32 ? bit : ~0u;
			if ((ZEND_TYPE_FULL_MASK(arg_info->type)
						& (MAY_BE_LONG | MAY_BE_DOUBLE))
					== (MAY_BE_LONG | MAY_BE_DOUBLE)) {
				plan->number_argument_mask |= bit;
			}
		}
	}
}

/* A definition that keeps its source's storage, machine kind and exact
 * type aliases the source's register. */
static void freeze_register_alias(
	zend_tpde_plan *plan, int32_t use_ssa, int32_t def_ssa) {
	if (use_ssa < 0 || def_ssa < 0) {
		return;
	}
	const int32_t source_index = zend_tpde_value_index(
		plan, zend_mir_value_from_original_ssa(
			static_cast<uint32_t>(use_ssa)));
	const int32_t result_index = zend_tpde_value_index(
		plan, zend_mir_value_from_original_ssa(
			static_cast<uint32_t>(def_ssa)));
	if (source_index < 0 || result_index < 0
			|| source_index == result_index) {
		return;
	}
	const zend_tpde_value &source = plan->values[source_index];
	const zend_tpde_value &result = plan->values[result_index];
	if (result.canonical_storage_id
				== source.canonical_storage_id
			&& result.machine_kind == source.machine_kind
			&& result.exact_type == source.exact_type) {
		plan->values[result_index].register_alias_value_index =
			source_index;
	}
}

/*
 * A receive opcode is an SSA definition but no machine operation: its
 * result is the incoming local-ABI argument. The plan keeps that edge,
 * so neither the adaptor analyzes Zend SSA nor the typed body reloads
 * the frame slot.
 */
static void freeze_receive_aliases(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	if (source_op_array != nullptr && source_ssa != nullptr
			&& source_ssa->ops != nullptr) {
		for (uint32_t opline = 0;
				opline < source_op_array->last; ++opline) {
			const zend_op &op = source_op_array->opcodes[opline];
			if (op.opcode == ZEND_VERIFY_RETURN_TYPE) {
				freeze_register_alias(plan,
					source_ssa->ops[opline].op1_use,
					source_ssa->ops[opline].op1_def);
				continue;
			}
			if (op.opcode != ZEND_RECV
					&& op.opcode != ZEND_RECV_INIT
					&& op.opcode != ZEND_RECV_VARIADIC) {
				continue;
			}
			const uint32_t argument = EX_VAR_TO_NUM(op.result.var);
			const int32_t result_ssa = source_ssa->ops[opline].result_def;
			if (argument >= plan->argument_count || result_ssa < 0
					|| plan->argument_value_indices[argument] < 0) {
				continue;
			}
			const int32_t result_index = zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					static_cast<uint32_t>(result_ssa)));
			if (result_index < 0) {
				continue;
			}
			const int32_t argument_index =
				plan->argument_value_indices[argument];
			const zend_tpde_value &incoming =
				plan->values[argument_index];
			const zend_tpde_value &received =
				plan->values[result_index];
			if (argument_index != result_index
					&& received.canonical_storage_id
						== incoming.canonical_storage_id
					&& received.machine_kind == incoming.machine_kind
					&& received.exact_type == incoming.exact_type) {
				plan->values[result_index].register_alias_value_index =
					argument_index;
			}
		}
	}
}

static void freeze_return_abi(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	if (source_op_array != nullptr
			&& source_op_array->arg_info != nullptr
			&& (source_op_array->fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		plan->return_abi = zend_tpde_local_abi_from_declared_type(
			&source_op_array->arg_info[-1].type,
			(source_op_array->fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0,
			ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED);
	} else {
		plan->return_abi =
			zend_tpde_local_abi_from_declared_type(
				nullptr, false, ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED);
	}
}

/* The blocks on a cycle of the CFG: strongly connected components of
 * more than one block, or a block that is its own successor. */
static void freeze_cyclic_blocks(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	std::vector<uint8_t> &cyclic_blocks = builder.cyclic_blocks;
	cyclic_blocks.assign(plan->block_count, 0);
	std::vector<uint8_t> visited_blocks(plan->block_count, 0);
	std::vector<uint32_t> finish_order;
	struct block_dfs_frame {
		uint32_t block;
		uint32_t next_edge;
	};
	std::vector<block_dfs_frame> dfs_stack;
	finish_order.reserve(plan->block_count);
	dfs_stack.reserve(plan->block_count);
	for (uint32_t start = 0; start < plan->block_count; ++start) {
		if (visited_blocks[start] != 0) {
			continue;
		}
		visited_blocks[start] = 1;
		dfs_stack.push_back({start, plan->block_successor_offsets[start]});
		while (!dfs_stack.empty()) {
			block_dfs_frame &frame = dfs_stack.back();
			const uint32_t end =
				plan->block_successor_offsets[frame.block + 1];
			if (frame.next_edge < end) {
				const uint32_t successor =
					plan->block_successors[frame.next_edge++];
				if (visited_blocks[successor] == 0) {
					visited_blocks[successor] = 1;
					dfs_stack.push_back({successor,
						plan->block_successor_offsets[successor]});
				}
				continue;
			}
			finish_order.push_back(frame.block);
			dfs_stack.pop_back();
		}
	}
	std::fill(visited_blocks.begin(), visited_blocks.end(), 0);
	std::vector<uint32_t> component;
	std::vector<uint32_t> component_stack;
	component.reserve(plan->block_count);
	component_stack.reserve(plan->block_count);
	for (auto order = finish_order.rbegin(); order != finish_order.rend();
			++order) {
		const uint32_t start = *order;
		if (visited_blocks[start] != 0) {
			continue;
		}
		component.clear();
		component_stack.push_back(start);
		visited_blocks[start] = 1;
		while (!component_stack.empty()) {
			const uint32_t block = component_stack.back();
			component_stack.pop_back();
			component.push_back(block);
			const uint32_t begin = plan->block_predecessor_offsets[block];
			const uint32_t end = plan->block_predecessor_offsets[block + 1];
			for (uint32_t edge = begin; edge < end; ++edge) {
				const uint32_t predecessor = plan->block_predecessors[edge];
				if (visited_blocks[predecessor] == 0) {
					visited_blocks[predecessor] = 1;
					component_stack.push_back(predecessor);
				}
			}
		}
		bool cyclic = component.size() > 1;
		if (!cyclic) {
			const uint32_t block = component[0];
			const uint32_t begin = plan->block_successor_offsets[block];
			const uint32_t end = plan->block_successor_offsets[block + 1];
			for (uint32_t edge = begin; edge < end; ++edge) {
				if (plan->block_successors[edge] == block) {
					cyclic = true;
					break;
				}
			}
		}
		if (cyclic) {
			for (const uint32_t block : component) {
				cyclic_blocks[block] = 1;
			}
		}
	}
}

static bool block_is_cyclic(
	const plan_builder &builder, zend_mir_block_id block_id) {
	const int32_t block_index = zend_tpde_block_index(builder.plan, block_id);
	return block_index >= 0
		&& builder.cyclic_blocks[static_cast<uint32_t>(block_index)] != 0;
}

static void sort_unique_storages(std::vector<zend_mir_storage_id> &storages) {
	std::sort(storages.begin(), storages.end());
	storages.erase(
		std::unique(storages.begin(), storages.end()), storages.end());
}

/* The storages a PHI defines, and those ASSIGN_REF binds. */
static void collect_phi_and_reference_storages(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	std::vector<zend_mir_storage_id> &phi_storages = builder.phi_storages;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &candidate = plan->instructions[i];
		if (candidate.record.opcode == ZEND_MIR_OPCODE_PHI
				&& zend_mir_id_is_valid(candidate.record.result_id)) {
			const int32_t result_index =
				zend_tpde_value_index(plan, candidate.record.result_id);
			if (result_index >= 0) {
				const zend_mir_storage_id storage_id =
					plan->values[result_index].canonical_storage_id;
				if (zend_mir_id_is_valid(storage_id)) {
					phi_storages.push_back(storage_id);
				}
			}
		}
	}
	sort_unique_storages(phi_storages);
	zend_tpde_collect_reference_assigned_storages(
		plan, builder.reference_assigned_storages);
}

static bool storage_has_phi(
	const plan_builder &builder, zend_mir_storage_id storage_id) {
	return std::binary_search(
		builder.phi_storages.begin(), builder.phi_storages.end(), storage_id);
}

static bool storage_assigned_by_reference(
	const plan_builder &builder, zend_mir_storage_id storage_id) {
	return std::binary_search(builder.reference_assigned_storages.begin(),
		builder.reference_assigned_storages.end(), storage_id);
}

static bool storage_lazy_capable(
	const plan_builder &builder, zend_mir_storage_id storage_id) {
	return builder.lazy_capable_function && zend_tpde_linux_inline_forms
		&& storage_id < builder.storage_may_alias.size()
		&& builder.storage_may_alias[storage_id] == 0
		&& !storage_assigned_by_reference(builder, storage_id);
}

[[nodiscard]]
static bool freeze_storage_alias(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	/*
	 * CVs that may hold a reference anywhere in the function or that Zend's
	 * SSA does not prove unaliased: a by-reference send, yield or return,
	 * ASSIGN_REF, global or static reads the slot without a boundary, so
	 * their scalar stores stay eager.
	 */
	std::vector<uint8_t> &storage_may_alias = builder.storage_may_alias;
	storage_may_alias.assign(
		source_op_array != nullptr
			? static_cast<size_t>(source_op_array->last_var) : 0, 0);
	if (source_ssa != nullptr && source_ssa->vars != nullptr
			&& source_ssa->var_info != nullptr) {
		for (int ssa = 0; ssa < source_ssa->vars_count; ++ssa) {
			const int variable = source_ssa->vars[ssa].var;
			if (variable < 0
					|| static_cast<size_t>(variable)
						>= storage_may_alias.size()) {
				continue;
			}
			/* Writers other than scalar assignment, compound assignment,
			 * increments and parameters update the slot themselves. */
			const int definition = source_ssa->vars[ssa].definition;
			bool foreign_writer = false;
			if (definition >= 0 && static_cast<uint32_t>(definition)
					< source_op_array->last) {
				switch (source_op_array->opcodes[definition].opcode) {
					case ZEND_ASSIGN:
					case ZEND_ASSIGN_OP:
					case ZEND_PRE_INC:
					case ZEND_PRE_DEC:
					case ZEND_POST_INC:
					case ZEND_POST_DEC:
					case ZEND_RECV:
					case ZEND_RECV_INIT:
						foreign_writer = source_ssa->ops != nullptr
							&& source_ssa->ops[definition].op1_def != ssa
							&& source_ssa->ops[definition].result_def != ssa;
						break;
					default:
						foreign_writer = true;
						break;
				}
			}
			if (foreign_writer || source_ssa->vars[ssa].alias != NO_ALIAS
					|| (source_ssa->var_info[ssa].type & MAY_BE_REF) != 0) {
				storage_may_alias[static_cast<size_t>(variable)] = 1;
			}
		}
	} else {
		std::fill(storage_may_alias.begin(), storage_may_alias.end(), 1);
	}
	/*
	 * File code's CVs are globals; a generator resumes from its slots;
	 * user opcode handlers observe every opline; catch and finally
	 * dispatch re-enter code without the boundaries' register state.
	 */
	builder.lazy_capable_function = source_op_array != nullptr
		&& source_op_array->function_name != nullptr
		&& source_op_array->last_try_catch == 0
		&& !plan->user_opcode_callbacks
		&& (source_op_array->fn_flags & ZEND_ACC_GENERATOR) == 0;
	/* Backtraces read parameters through the frame. */
	const uint32_t parameter_storage_count = source_op_array == nullptr ? 0
		: source_op_array->num_args
			+ ((source_op_array->fn_flags & ZEND_ACC_VARIADIC) != 0 ? 1 : 0);
	if (!storage_may_alias.empty()) {
		if (!plan_alloc(plan->operand_observed_storages,
				storage_may_alias.size(), diag,
				"unable to freeze operand-observed storages")) {
			return false;
		}
		plan->operand_observed_storage_count =
			static_cast<uint32_t>(storage_may_alias.size());
		for (uint32_t storage = parameter_storage_count;
				storage < storage_may_alias.size(); ++storage) {
			plan->operand_observed_storages[storage] =
				storage_lazy_capable(builder, storage) ? 1 : 0;
		}
	}
	return true;
}

/* The value reaching a zval store's destination, when it is an unaliased
 * non-refcounted scalar already in the destination's storage. */
static const zend_tpde_value *unaliased_scalar_previous(
	const plan_builder &builder, uint32_t i, int32_t ssa) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_ssa *const source_ssa = builder.source_ssa;
	const int32_t index = zend_tpde_value_index(
		plan, zend_mir_value_from_original_ssa(
			static_cast<uint32_t>(ssa)));
	if (index < 0 || source_ssa->vars[ssa].alias != NO_ALIAS) {
		return nullptr;
	}
	const zend_tpde_value &previous = plan->values[index];
	return previous.canonical_storage_id
				== plan->instructions[i].zval_store_storage_id
			&& previous.category
				== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
			&& !previous.canonical_alias_observable
		? &previous : nullptr;
}

[[nodiscard]]
static bool plan_zval_store(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	if (count != 2) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"zval store requires a scalar source and destination identity");
		return false;
	}
	const zend_mir_value_id source_id = zend_tpde_operand_at(
		plan, &plan->instructions[i], 0);
	const zend_mir_value_id destination_id = zend_tpde_operand_at(
		plan, &plan->instructions[i], 1);
	const int32_t source_index =
		zend_tpde_value_index(plan, source_id);
	const int32_t destination_index =
		zend_tpde_value_index(plan, destination_id);
	bool loop_carried_integer_transport = false;
	if (source_index < 0 || destination_index < 0
			|| !zend_mir_scalar_type_is_exact(
				plan->values[source_index].exact_type)
			|| !zend_mir_id_is_valid(
				plan->values[destination_index]
					.canonical_storage_id)) {
		char message[192];
		std::snprintf(message, sizeof(message),
			"zval store %u operands source=%u index=%d type=%u "
			"destination=%u index=%d storage=%u lack metadata",
			record.id, source_id, source_index,
			source_index >= 0
				? static_cast<unsigned>(
					plan->values[source_index].exact_type)
				: 0,
			destination_id, destination_index,
			destination_index >= 0
				? plan->values[destination_index]
					.canonical_storage_id
				: ZEND_MIR_ID_INVALID);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			message);
		return false;
	}
	plan->instructions[i].zval_store_storage_id =
		plan->values[destination_index].canonical_storage_id;
	/*
	 * ZVAL_STORE overwrites the value reaching op1 at its source
	 * assignment. When that SSA value is an unaliased non-refcounted
	 * scalar, no reference resolution, destructor or refcount change
	 * is observable. The reaching definition is the proof, not the
	 * slot's history: a dominating assignment may already have
	 * released an earlier refcounted value. On Linux x64 the typed
	 * tier also stores the results of other scalar definitions; the
	 * reaching value is the use paired with the stored definition.
	 */
	const zend_ssa_op *store_ssa_op =
		source_op_array != nullptr && source_ssa != nullptr
			&& source_ssa->ops != nullptr
			&& record.source_position_id < source_op_array->last
		? &source_ssa->ops[record.source_position_id] : nullptr;
	const int destination_ssa =
		zend_mir_value_is_original_ssa(destination_id)
		? static_cast<int>(destination_id) : -1;
	const bool assign_store = store_ssa_op != nullptr
		&& source_op_array->opcodes[
			record.source_position_id].opcode == ZEND_ASSIGN;
	int32_t reaching_ssa = -1;
	if (assign_store) {
		reaching_ssa = store_ssa_op->op1_use;
	} else if (store_ssa_op != nullptr && zend_tpde_linux_inline_forms
			&& destination_ssa >= 0) {
		reaching_ssa = store_ssa_op->op1_def == destination_ssa
			? store_ssa_op->op1_use
			: store_ssa_op->result_def == destination_ssa
				? store_ssa_op->result_use
				: -1;
	}
	/* Only ASSIGN stores may become lazy; other producers qualify for
	 * a plain store alone. */
	bool plain_scalar_destination = false;
	if (reaching_ssa >= 0 && source_ssa->vars != nullptr
			&& !assign_store) {
		plain_scalar_destination =
			unaliased_scalar_previous(builder, i, reaching_ssa) != nullptr;
		reaching_ssa = -1;
	}
	if (reaching_ssa >= 0 && source_ssa->vars != nullptr) {
		if (const zend_tpde_value *previous =
				unaliased_scalar_previous(builder, i, reaching_ssa)) {
			plan->instructions[i].zval_store_direct_scalar = true;
			loop_carried_integer_transport =
				previous->exact_type == ZEND_MIR_SCALAR_TYPE_I64
				&& plan->values[source_index].category
					== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				&& plan->values[source_index].exact_type
					== ZEND_MIR_SCALAR_TYPE_I64
				&& plan->values[source_index].canonical_storage_id
					== plan->instructions[i].zval_store_storage_id;
		}
	}
	if (plan->instructions[i].zval_store_direct_scalar) {
		const bool phi_storage = storage_has_phi(builder,
			plan->instructions[i].zval_store_storage_id);
		/*
		 * An integer assignment that defines the loop-carried value
		 * need not publish the preceding PHI. Other PHI-backed stores
		 * stay eager: their source may not dominate every guarded
		 * continuation. A constant has no register definition that a
		 * boundary could store.
		 */
		plan->instructions[i].zval_store_lazy_scalar =
			!plan->values[source_index].constant
			&& (block_is_cyclic(builder, record.block_id)
				|| (storage_lazy_capable(builder,
						plan->instructions[i].zval_store_storage_id)
					&& plan->values[source_index].category
						== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
					&& zend_mir_scalar_type_is_exact(
						plan->values[source_index].exact_type)
					&& plan->values[source_index].exact_type
						!= ZEND_MIR_SCALAR_TYPE_NULL))
			&& (!phi_storage || loop_carried_integer_transport);
	}
	/*
	 * A temporary or VAR result slot never owns a value that the
	 * store must release (the VM writes result slots without
	 * releasing them either), nor does a CV whose reaching value is
	 * an unaliased non-refcounted scalar.
	 */
	plan->instructions[i].zval_store_plain =
		zend_tpde_linux_inline_forms
		&& !plan->instructions[i].zval_store_lazy_scalar
		&& (plan->instructions[i].zval_store_direct_scalar
			|| plain_scalar_destination
			|| (source_op_array != nullptr
				&& plan->instructions[i].zval_store_storage_id
					>= static_cast<uint32_t>(
						source_op_array->last_var)));
	if (!plan->instructions[i].zval_store_lazy_scalar
			&& !plan->instructions[i].zval_store_plain) {
		plan->instructions[i].runtime_helper =
			ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW;
		require_runtime_helper(
			plan, plan->instructions[i].runtime_helper);
	}
	return true;
}

/*
 * A long ASSIGN_OP or INCDEC without a result on an unaliased CV definition
 * mutates its register and stores the slot lazily.
 */
static void freeze_lazy_mutation(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	if ((record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_INCDEC)
			&& (block_is_cyclic(builder, record.block_id)
				|| (plan->instructions[i].value_operation
						.op1_storage_id
						< builder.storage_may_alias.size()
					&& storage_lazy_capable(builder,
						plan->instructions[i].value_operation
							.op1_storage_id)))) {
		const zend_mir_executable_value_ref &operation =
			plan->instructions[i].value_operation;
		const uint32_t mutation_ssa =
			zend_tpde_register_mutation_ssa_plus_one(
				plan->instructions[i], true);
		const int32_t mutation_value =
			mutation_ssa == 0 ? -1
			: zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					mutation_ssa - 1));
		if (source_op_array != nullptr
				&& source_op_array->function_name != nullptr
				&& zend_mir_id_is_valid(
					operation.op1_storage_id)
				&& !storage_assigned_by_reference(builder,
					operation.op1_storage_id)
				&& mutation_value >= 0
				&& source_ssa != nullptr
				&& source_ssa->vars != nullptr
				&& mutation_ssa - 1
					< static_cast<uint32_t>(
						source_ssa->vars_count)
				&& source_ssa->vars[mutation_ssa - 1].alias
					== NO_ALIAS) {
			const zend_tpde_value &value =
				plan->values[
					static_cast<uint32_t>(mutation_value)];
			if (value.canonical_storage_id
					== operation.op1_storage_id) {
				/*
				 * NO_ALIAS proves this CV definition is not observable
				 * through a zend_reference. The value-location model
				 * may keep canonical observability for the polymorphic
				 * cold result; the long fast path still need not
				 * publish on every loop edge.
				 */
				plan->instructions[i].mutation_storage_id =
					operation.op1_storage_id;
				plan->instructions[i].mutation_lazy_scalar = true;
			}
		}
	}
}

[[nodiscard]]
static bool plan_multi_branch(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	zend_tpde_multi_branch layout;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	if (!zend_tpde_multi_branch_at(
			plan, plan->instructions[i], record, &layout)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"multiway branch lacks exact source-backed cases");
		return false;
	}
	if (layout.source_opcode == ZEND_MATCH
			&& (operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.op1.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_VALUE_CHECK_VAR);
	}
	return true;
}

[[nodiscard]]
static bool plan_semantic_echo(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	zend_mir_value_id value_id;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	if (operation.source_opcode != ZEND_ECHO
			|| !source_operand_value_id(operation.op1, value_id)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"semantic echo lacks an explicit scalar operand");
		return false;
	}
	const int32_t value_index =
		zend_tpde_value_index(plan, value_id);
	if (value_index < 0
			|| !zend_mir_scalar_type_is_exact(
				plan->values[value_index].exact_type)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
			"semantic echo operand has no exact scalar type");
		return false;
	}
	plan->instructions[i].source_effect_exact_type =
		plan->values[value_index].exact_type;
	plan->instructions[i].runtime_helper =
		plan->values[value_index].exact_type
			== ZEND_MIR_SCALAR_TYPE_F64
			? ZEND_NATIVE_HELPER_ECHO_DOUBLE
			: ZEND_NATIVE_HELPER_ECHO_INTEGER;
	require_runtime_helper(plan,
		plan->instructions[i].runtime_helper);
	return true;
}

static void refine_long_binary_result(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_ssa *const source_ssa = builder.source_ssa;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	zend_tpde_long_binary binary{};
	zend_mir_value_id result_id;
	const bool long_operands =
		source_operand_exact_type(plan, operation.op1)
			== ZEND_MIR_SCALAR_TYPE_I64
		&& source_operand_exact_type(plan, operation.op2)
			== ZEND_MIR_SCALAR_TYPE_I64;
	/* Addition and subtraction of longs may overflow into a
	 * double on the cold path: only inference proves a long. */
	const bool may_overflow =
		(operation.source_opcode == ZEND_ADD
			|| operation.source_opcode == ZEND_SUB)
		&& (source_ssa == nullptr || source_ssa->var_info == nullptr
			|| operation.result.ssa_variable_id
				>= static_cast<uint32_t>(source_ssa->vars_count)
			|| (source_ssa->var_info[
					operation.result.ssa_variable_id].type
				& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF))
				!= MAY_BE_LONG);
	if (long_operands && !may_overflow
			&& zend_tpde_long_binary_at(
				plan->instructions[i], &binary)
			&& source_operand_value_id(
				operation.result, result_id)) {
		const int32_t result_index =
			zend_tpde_value_index(plan, result_id);
		if (result_index >= 0) {
			plan->values[result_index].exact_type =
				operation.source_opcode == ZEND_ADD
					|| operation.source_opcode == ZEND_SUB
					|| operation.source_opcode == ZEND_BW_OR
					|| operation.source_opcode == ZEND_BW_AND
					|| operation.source_opcode == ZEND_BW_XOR
				? ZEND_MIR_SCALAR_TYPE_I64
				: ZEND_MIR_SCALAR_TYPE_I1;
		}
	}
}

[[nodiscard]]
static bool plan_verify_return_type(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	zend_mir_value_id verified_value_id = ZEND_MIR_ID_INVALID;
	if (operation.source_opcode != ZEND_VERIFY_RETURN_TYPE
			|| operation.op2.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| (operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"return type verification lacks explicit source operands");
		return false;
	}
	/*
	 * An exact scalar that satisfies the declared return type
	 * cannot take Zend's coercion or TypeError path and needs no
	 * VERIFY_RETURN_TYPE helper.
	 */
	if (source_op_array != nullptr
			&& source_op_array->arg_info != nullptr
			&& (source_op_array->fn_flags
					& ZEND_ACC_HAS_RETURN_TYPE) != 0
			&& source_operand_value_id(
				operation.op1, verified_value_id)) {
		const int32_t verified_value_index =
			zend_tpde_value_index(plan, verified_value_id);
		if (verified_value_index >= 0
				&& zend_mir_scalar_type_is_exact(
					plan->values[
						verified_value_index].exact_type)
				&& exact_scalar_satisfies_type(
					plan->values[
						verified_value_index].exact_type,
					source_op_array->arg_info[-1].type)) {
			plan->instructions[i].runtime_helper =
				ZEND_NATIVE_HELPER_COUNT;
			return true;
		}
	}
	require_runtime_helper(
		plan, ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE);
	return true;
}

[[nodiscard]]
static bool plan_argument_introspection(
	plan_builder &builder, uint32_t i, zend_native_runtime_helper_id helper) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	const bool valid_op1 =
		record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS
			? operation.op1.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED
			: (operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL);
	if (!valid_op1
			|| operation.op2.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.result.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.auxiliary.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.source_opcode
				!= (record.opcode
						== ZEND_MIR_OPCODE_FUNC_NUM_ARGS
					? ZEND_FUNC_NUM_ARGS
					: ZEND_FUNC_GET_ARGS)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"argument introspection lacks explicit source operands");
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS) {
		zend_mir_value_id result_id;
		const int32_t result_index =
			source_operand_value_id(operation.result, result_id)
				? zend_tpde_value_index(plan, result_id)
				: -1;
		if (result_index < 0 || result_id != record.result_id) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"func_num_args result identity is inconsistent");
			return false;
		}
		/*
		 * FUNC_NUM_ARGS always yields an integer; the SSA overlay
		 * may have no fact for this temporary.
		 */
		plan->values[result_index].exact_type =
			ZEND_MIR_SCALAR_TYPE_I64;
		if (record.representation
				!= ZEND_MIR_REPRESENTATION_I64) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"func_num_args result representation is not i64");
			return false;
		}
		/*
		 * The count belongs to the active invocation, even when
		 * analysis saw only fixed-arity callers: it is read from
		 * the frame.
		 */
		plan->values[result_index].constant = false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_FUNC_GET_ARGS) {
		require_runtime_helper(plan, helper);
	}
	return true;
}

/* The helpers a value operation's inline and address forms reach. */
[[nodiscard]]
static bool require_value_helpers(
	plan_builder &builder, uint32_t i, zend_native_runtime_helper_id helper) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	zend_tpde_dim_direct dim_direct{};
	if (zend_tpde_linux_inline_forms
			&& zend_tpde_dim_direct_at(
				plan->instructions[i], &dim_direct)) {
		require_runtime_helper(plan,
			record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				? ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R_DIRECT
				: record.opcode
						== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
					? ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM_DIRECT
					: ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_ADDRESS);
	}
	if (zend_tpde_linux_inline_forms) {
		/* The address forms of explicit-operand helpers. */
		switch (record.opcode) {
			case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY_ADDRESS);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_NEW_ARRAY);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_HASH_REAL_INIT_MIXED);
				[[fallthrough]];
			case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT_ADDRESS);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_HASH_UPDATE);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_HASH_INDEX_UPDATE);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_HASH_NEXT_INDEX_INSERT);
				break;
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_CHAR_STRINGS);
				break;
			default:
				break;
		}
	}
	zend_tpde_concat_direct identical_direct{};
	if (zend_tpde_linux_inline_forms
			&& zend_tpde_identical_direct_at(
				plan->instructions[i], &identical_direct)) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_VALUE_IDENTICAL_DIRECT);
	}
	zend_tpde_concat_direct concat_direct{};
	if (zend_tpde_linux_inline_forms
			&& zend_tpde_concat_direct_at(
				plan->instructions[i], &concat_direct)) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_VALUE_CONCAT_DIRECT);
		for (const zend_native_runtime_helper_id strings : {
				ZEND_NATIVE_HELPER_CONCAT_STRINGS_VV,
				ZEND_NATIVE_HELPER_CONCAT_STRINGS_TV,
				ZEND_NATIVE_HELPER_CONCAT_STRINGS_VT,
				ZEND_NATIVE_HELPER_CONCAT_STRINGS_TT}) {
			require_runtime_helper(plan, strings);
		}
	}
	zend_tpde_concat_assign_direct concat_assign_direct{};
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
			&& zend_tpde_linux_inline_forms
			&& zend_tpde_concat_assign_direct_at(
				plan->instructions[i], &concat_assign_direct)) {
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_VALUE_CONCAT_ASSIGN_DIRECT);
	}
	if (record.opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL) {
		const zend_mir_executable_value_ref &operation =
			plan->instructions[i].value_operation;
		if (operation.source_opcode < ZEND_FRAMELESS_ICALL_0
				|| operation.source_opcode > ZEND_FRAMELESS_ICALL_3
				|| operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"frameless internal call lacks explicit operands");
			return false;
		}
		zend_tpde_frameless_direct frameless_direct{};
		if (zend_tpde_linux_inline_forms
				&& zend_tpde_frameless_direct_at(
					plan->instructions[i], &frameless_direct)) {
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_CALL_FRAMELESS_DIRECT);
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_CALL_FRAMELESS_2_ADDRESS);
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_CALL_FRAMELESS_3_ADDRESS);
			if (operation.source_opcode == ZEND_FRAMELESS_ICALL_1) {
				require_runtime_helper(
					plan, ZEND_NATIVE_HELPER_CALL_FRAMELESS_1);
			}
		}
		const uint32_t argument_count =
			operation.source_opcode - ZEND_FRAMELESS_ICALL_0;
		const zend_mir_source_operand_ref arguments[] = {
			operation.op1, operation.op2, operation.auxiliary};
		for (uint32_t argument = 0; argument < 3; ++argument) {
			if ((argument < argument_count)
					== (arguments[argument].kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"frameless internal call arity disagrees with operands");
				return false;
			}
		}
	}
	require_runtime_helper(plan, helper);
	if (helper == ZEND_NATIVE_HELPER_VALUE_CHECK_UNDEF_ARGS) {
		/* Generated code tests the frame's flag first. */
		require_runtime_helper(
			plan, ZEND_NATIVE_HELPER_CALL_CHECK_UNDEF_FRAME);
	}
	if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL) {
		const zend_mir_executable_value_ref &operation =
			plan->instructions[i].value_operation;
		if (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (operation.extended_value == ZEND_INCLUDE_ONCE
					|| operation.extended_value
						== ZEND_REQUIRE_ONCE)) {
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE);
		}
	}
	return true;
}

[[nodiscard]]
static plan_status plan_value_operation(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	const bool semantic_echo =
		record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR;
	const bool multi_branch =
		record.opcode == ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH;
	const zend_native_runtime_helper_id helper = semantic_echo
		? ZEND_NATIVE_HELPER_COUNT
		: multi_branch
			? ZEND_NATIVE_HELPER_COUNT
		: executable_value_helper(record.opcode);
	plan->instructions[i].runtime_helper = helper;
	if ((semantic_echo ? count != 1 : count != 0)
			|| !zend_mir_id_is_valid(record.source_position_id)
			|| (!semantic_echo && !multi_branch
				&& helper == ZEND_NATIVE_HELPER_COUNT)
			|| (!semantic_echo && !multi_branch
				&& !zend_tpde_helper_has_explicit_operands(helper))
			|| !plan->instructions[i].has_value_operation
			|| plan->instructions[i].value_operation.opcode
				!= record.opcode
			|| plan->instructions[i].value_operation.source_position_id
				!= record.source_position_id) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"executable value operation lacks exact source semantics");
		return plan_status::failed;
	}
	freeze_lazy_mutation(builder, i);
	if (multi_branch) {
		return plan_multi_branch(builder, i)
			? plan_status::finished : plan_status::failed;
	}
	if (semantic_echo) {
		return plan_semantic_echo(builder, i)
			? plan_status::finished : plan_status::failed;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
		refine_long_binary_result(builder, i);
	}
	if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
		return plan_verify_return_type(builder, i)
			? plan_status::finished : plan_status::failed;
	}
	if (record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS
			|| record.opcode == ZEND_MIR_OPCODE_FUNC_GET_ARGS) {
		return plan_argument_introspection(builder, i, helper)
			? plan_status::finished : plan_status::failed;
	}
	if (record.opcode >= ZEND_MIR_OPCODE_GENERATOR_CREATE
			&& record.opcode <= ZEND_MIR_OPCODE_GENERATOR_RETURN) {
		const zend_mir_executable_value_ref &operation =
			plan->instructions[i].value_operation;
		const uint32_t expected_source_opcode =
			record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
				? ZEND_GENERATOR_CREATE
				: record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					? ZEND_YIELD
					: record.opcode
							== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM
						? ZEND_YIELD_FROM
						: ZEND_GENERATOR_RETURN;
		if (operation.source_opcode != expected_source_opcode
				|| source_op_array == nullptr
				|| (source_op_array->fn_flags
					& ZEND_ACC_GENERATOR) == 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator operation lacks a generator source frame");
			return plan_status::failed;
		}
		require_runtime_helper(plan, helper);
		return plan_status::finished;
	}
	return require_value_helpers(builder, i, helper)
		? plan_status::continues : plan_status::failed;
}

[[nodiscard]]
static bool plan_source_zval_return(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	zend_tpde_instruction &instruction = plan->instructions[i];
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	zend_mir_value_id value_id;
	if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
			|| !instruction.has_value_operation
			|| operation.opcode != record.opcode
			|| (operation.source_opcode != ZEND_RETURN
				&& operation.source_opcode != ZEND_RETURN_BY_REF)
			|| operation.source_position_id
				!= record.source_position_id
			|| (operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				? operation.op1.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_KIND_INVALID
					|| operation.op1_storage_id
						!= ZEND_MIR_ID_INVALID
				: (operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| operation.op1.slot_kind
						< ZEND_MIR_SOURCE_SLOT_CV
					|| operation.op1.slot_kind
						> ZEND_MIR_SOURCE_SLOT_VAR
					|| operation.op1_storage_id
						== ZEND_MIR_ID_INVALID)
			|| operation.op2.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.result.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source-zval return lacks explicit source semantics");
		return false;
	}
	const int32_t value_index =
		source_operand_value_id(operation.op1, value_id)
			? zend_tpde_value_index(plan, value_id) : -1;
	if (operation.source_opcode == ZEND_RETURN
			&& operation.op1.kind
				!= ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& value_index >= 0
			/*
			 * A scalar fact on a frame argument describes the compiled
			 * source, not every invocation: copying the raw payload
			 * would skip zval ownership for an array or object
			 * argument. Arguments return through the helper.
			 */
			&& plan->values[value_index].argument_index < 0
			&& (source_ssa == nullptr
				|| source_ssa->var_info == nullptr
				|| operation.op1.ssa_variable_id
					== ZEND_MIR_ID_INVALID
				|| operation.op1.ssa_variable_id
					>= static_cast<uint32_t>(source_ssa->vars_count)
				|| (source_ssa->var_info[
						operation.op1.ssa_variable_id].type
						& MAY_BE_UNDEF) == 0)
			&& zend_mir_scalar_type_is_exact(
				plan->values[value_index].exact_type)) {
		bool helper_mutates_return_storage = false;
		for (uint32_t previous = 0; previous < i; ++previous) {
			const zend_tpde_instruction &candidate =
				plan->instructions[previous];
			zend_mir_storage_id call_result_storage =
				ZEND_MIR_ID_INVALID;
			if (candidate.direct_call != nullptr) {
				call_result_storage = source_descriptor_storage(
					source_op_array,
					candidate.direct_call->result_operand);
			} else if (candidate.direct_internal_call != nullptr) {
				call_result_storage = source_descriptor_storage(
					source_op_array,
					candidate.direct_internal_call->result_operand);
			} else if (candidate.user_call != nullptr) {
				call_result_storage = source_descriptor_storage(
					source_op_array,
					candidate.user_call->do_result);
			}
			if (call_result_storage == operation.op1_storage_id) {
				helper_mutates_return_storage = true;
				break;
			}
			if (!candidate.has_value_operation
					|| candidate.runtime_helper
						== ZEND_NATIVE_HELPER_COUNT) {
				continue;
			}
			const zend_mir_executable_value_ref &candidate_operation =
				candidate.value_operation;
			if (candidate_operation.op1_storage_id
						== operation.op1_storage_id
					|| candidate_operation.op2_storage_id
						== operation.op1_storage_id
					|| candidate_operation.result_storage_id
						== operation.op1_storage_id
					|| candidate_operation.auxiliary_storage_id
						== operation.op1_storage_id) {
				helper_mutates_return_storage = true;
				break;
			}
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT}
				+ operation.op1_storage_id) * sizeof(zval);
		if (source_offset > UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-zval return offset exceeds the frame ABI");
			return false;
		}
		if (!helper_mutates_return_storage) {
			instruction.direct_scalar_return = true;
			instruction.direct_scalar_return_offset =
				static_cast<uint32_t>(source_offset);
			instruction.runtime_helper = ZEND_NATIVE_HELPER_COUNT;
		}
	}
	return true;
}

[[nodiscard]]
static bool plan_source_zval_throw(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
			|| !plan->instructions[i].has_value_operation
			|| operation.opcode != record.opcode
			|| operation.source_opcode != ZEND_THROW
			|| operation.source_position_id
				!= record.source_position_id
			|| operation.op1.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.op2.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.result.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source-zval throw lacks exact source semantics");
		return false;
	}
	plan->instructions[i].runtime_helper =
		ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL;
	require_runtime_helper(
		plan, plan->instructions[i].runtime_helper);
	return true;
}

[[nodiscard]]
static bool plan_iterator_branch(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	const zend_mir_executable_value_ref &operation =
		plan->instructions[i].value_operation;
	const bool iterator_opcode =
		operation.source_opcode == ZEND_FE_RESET_R
		|| operation.source_opcode == ZEND_FE_RESET_RW
		|| operation.source_opcode == ZEND_FE_FETCH_R
		|| operation.source_opcode == ZEND_FE_FETCH_RW;
	if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
			|| !plan->instructions[i].has_value_operation
			|| operation.opcode != record.opcode
			|| operation.source_position_id
				!= record.source_position_id
			|| !iterator_opcode) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"iterator branch lacks explicit source semantics");
		return false;
	}
	plan->instructions[i].runtime_helper =
		ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
	require_runtime_helper(
		plan, plan->instructions[i].runtime_helper);
	return true;
}

[[nodiscard]]
static bool plan_boxed_cond_branch(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	const uint32_t count = plan->instructions[i].operand_count;
	const bool boxed_cond_branch =
		(record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
			|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
			|| record.opcode
				== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
			|| record.opcode
				== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH)
		&& plan->instructions[i].has_value_operation
		&& (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
			? plan->instructions[i].value_operation.opcode
				== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
			: plan->instructions[i].value_operation.opcode
				== record.opcode);
	if (boxed_cond_branch) {
		const zend_mir_executable_value_ref &operation =
			plan->instructions[i].value_operation;
		const bool compatible_scalar_branch =
			record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
			&& count == 1;
		const bool conditional_opcode =
			(record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				? operation.source_opcode
					== ZEND_BIND_INIT_STATIC_OR_JMP
				: record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
					? operation.source_opcode == ZEND_JMP_FRAMELESS
					: operation.source_opcode == ZEND_JMPZ
			|| operation.source_opcode == ZEND_JMPNZ
			|| operation.source_opcode == ZEND_JMPZ_EX
			|| operation.source_opcode == ZEND_JMPNZ_EX
			|| operation.source_opcode == ZEND_JMP_SET
			|| operation.source_opcode == ZEND_COALESCE
			|| operation.source_opcode == ZEND_JMP_NULL
			|| operation.source_opcode == ZEND_ASSERT_CHECK);
		const bool exact_operands =
			record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				? (operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV
					&& operation.op2.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& operation.result.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
				: record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
					? operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& operation.op2.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
					: operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| operation.source_opcode
							== ZEND_ASSERT_CHECK;
		if ((!compatible_scalar_branch && count != 0)
				|| !zend_mir_id_is_valid(record.source_position_id)
				|| !plan->instructions[i].has_value_operation
				|| (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
					? operation.opcode
						!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					: operation.opcode != record.opcode)
				|| operation.source_position_id
					!= record.source_position_id
				|| !exact_operands
				|| !conditional_opcode) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source value branch lacks exact source semantics");
			return false;
		}
		plan->instructions[i].runtime_helper =
			record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				? ZEND_NATIVE_HELPER_VALUE_BIND_STATIC_BRANCH
				: record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
					? ZEND_NATIVE_HELPER_VALUE_FRAMELESS_BRANCH
					: ZEND_NATIVE_HELPER_VALUE_COND_BRANCH;
		require_runtime_helper(
			plan, plan->instructions[i].runtime_helper);
	}
	return true;
}

[[nodiscard]]
static bool plan_interrupt_statepoint(plan_builder &builder, uint32_t i) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	zend_native_diagnostic *const diag = builder.diag;
	const zend_mir_instruction_record &record =
		plan->instructions[i].record;
	bool &interrupt_frames_read = builder.interrupt_frames_read;
	std::vector<zend_mir_frame_state_ref> &interrupt_frames =
		builder.interrupt_frames;
	std::vector<zend_tpde_id_index_entry> &interrupt_frame_index =
		builder.interrupt_frame_index;
	uint32_t &interrupt_frame_index_capacity =
		builder.interrupt_frame_index_capacity;
	if (!interrupt_frames_read) {
		/* The frame states by ID, read once; the first of an ID. */
		interrupt_frames_read = true;
		const uint32_t frame_count =
			view->frame_state_count(view->context);
		interrupt_frames.resize(frame_count);
		interrupt_frame_index_capacity =
			frame_count == 0 ? 0 : id_index_capacity(frame_count);
		interrupt_frame_index.assign(interrupt_frame_index_capacity,
			{ZEND_MIR_ID_INVALID, 0});
		for (uint32_t n = 0; n < frame_count; ++n) {
			if (!view->frame_state_at(
					view->context, n, &interrupt_frames[n])) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR frame-state table is unreadable");
				return false;
			}
			if (zend_mir_id_is_valid(interrupt_frames[n].id)
					&& id_index_find(interrupt_frame_index.data(),
						interrupt_frame_index_capacity,
						interrupt_frames[n].id) < 0) {
				id_index_insert(interrupt_frame_index.data(),
					interrupt_frame_index_capacity,
					interrupt_frames[n].id, n);
			}
		}
	}
	const int32_t frame_position = zend_mir_id_is_valid(
			record.frame_state_id)
		? id_index_find(interrupt_frame_index.data(),
			interrupt_frame_index_capacity, record.frame_state_id)
		: -1;
	const bool found_frame = frame_position >= 0;
	const zend_mir_frame_state_ref frame = found_frame
		? interrupt_frames[static_cast<uint32_t>(frame_position)]
		: zend_mir_frame_state_ref{};
	if (!found_frame || frame.function_id != plan->function.id) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"interrupt statepoint lacks its source-backed frame");
		return false;
	}
	plan->instructions[i].source_opline_index = frame.opline_index;
	require_runtime_helper(plan, ZEND_NATIVE_HELPER_INTERRUPT_POLL);
	return true;
}

[[nodiscard]]
static bool plan_instructions(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	builder.sites = plan->call_sites;
	builder.site_count = plan->call_site_count;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record &record =
			plan->instructions[i].record;
		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			if (!plan_zval_store(builder, i)) {
				return false;
			}
			continue;
		}
		if (zend_mir_opcode_is_executable_value(record.opcode)) {
			const plan_status status = plan_value_operation(builder, i);
			if (status == plan_status::failed) {
				return false;
			}
			if (status == plan_status::finished) {
				continue;
			}
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& !plan_source_zval_return(builder, i)) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				&& !plan_source_zval_throw(builder, i)) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH
				&& !plan_iterator_branch(builder, i)) {
			return false;
		}
		if (!plan_boxed_cond_branch(builder, i)) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				&& (record.effects & ZEND_MIR_EFFECT_MASK(
					ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0
				&& !plan_interrupt_statepoint(builder, i)) {
			return false;
		}
		switch (record.opcode) {
			case ZEND_MIR_OPCODE_CATCH_ENTER:
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_CATCH_ENTER);
				break;
			case ZEND_MIR_OPCODE_FINALLY_ENTER:
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_ENTER);
				break;
			case ZEND_MIR_OPCODE_FINALLY_CALL:
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_CALL);
				break;
			case ZEND_MIR_OPCODE_FINALLY_RETURN:
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_RETURN);
				break;
			case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
				if (!plan->instructions[i].direct_scalar_return) {
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL);
				}
				break;
			default:
				break;
		}
		if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
				&& !plan_call_site(builder, i)) {
			return false;
		}
	}
	return true;
}

/* The source effects: exception routes and debug probes. */
[[nodiscard]]
static bool freeze_source_effects(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_native_source_effect *const effects = builder.effects;
	const uint32_t effect_count = builder.effect_count;
	zend_native_diagnostic *const diag = builder.diag;
	std::vector<uint32_t> source_instruction_offsets(
		static_cast<size_t>(plan->source_opcode_count) + 1, 0);
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[i]);

		if (zend_mir_id_is_valid(record.source_position_id)
				&& record.source_position_id < plan->source_opcode_count) {
			source_instruction_offsets[record.source_position_id + 1]++;
		}
	}
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		source_instruction_offsets[source + 1] +=
			source_instruction_offsets[source];
	}
	std::vector<uint32_t> source_instruction_indexes(
		source_instruction_offsets[plan->source_opcode_count]);
	std::vector<uint32_t> source_instruction_cursors =
		source_instruction_offsets;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[i]);

		if (zend_mir_id_is_valid(record.source_position_id)
				&& record.source_position_id < plan->source_opcode_count) {
			source_instruction_indexes[
				source_instruction_cursors[record.source_position_id]++] = i;
		}
	}
	for (uint32_t i = 0; i < effect_count; ++i) {
		const zend_native_source_effect &effect = effects[i];
		zend_tpde_instruction *match = nullptr;
		bool exception_match = false;
		zend_mir_opcode source_opcode = ZEND_MIR_OPCODE_INVALID;
		uint32_t source_instruction_begin = 0;
		uint32_t source_instruction_end = 0;

		if (!zend_mir_id_is_valid(effect.source_position_id)
				|| (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE
					? !zend_mir_id_is_valid(effect.target_block_id)
					: effect.kind != ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"source effect is invalid");
			return false;
		}
		if (effect.source_position_id < plan->source_opcode_count) {
			source_instruction_begin =
				source_instruction_offsets[effect.source_position_id];
			source_instruction_end =
				source_instruction_offsets[effect.source_position_id + 1];
		}
		for (uint32_t source_instruction = source_instruction_begin;
				source_instruction < source_instruction_end;
				++source_instruction) {
			const uint32_t n =
				source_instruction_indexes[source_instruction];
			zend_tpde_instruction &candidate = plan->instructions[n];
			const zend_mir_instruction_record candidate_record =
				zend_tpde_instruction_record_at(plan, &candidate);
			const bool boxed_cond_branch = candidate.has_value_operation
				&& candidate_record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& candidate.value_operation.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH;
			source_opcode = candidate_record.opcode;
			if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
				if (candidate_record.opcode
						== ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH) {
					/*
					 * The inline dispatch itself cannot raise. Its source-backed
					 * fallback is a separate successor instruction with its own
					 * exception route.
					 */
					exception_match = true;
					continue;
				}
				if (!boxed_cond_branch
						&& executable_value_helper(candidate_record.opcode)
						== ZEND_NATIVE_HELPER_COUNT
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_ITERATOR_BRANCH
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_CATCH_ENTER
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_CREATE
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_YIELD
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM) {
					continue;
				}
				/*
				 * The value helpers one source opcode lowers to share its try
				 * region and so its exception successor.
				 */
				if (zend_mir_id_is_valid(candidate.exception_block_id)
						&& candidate.exception_block_id
							!= effect.target_block_id) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"value exception routes disagree for one source opcode");
					return false;
				}
				candidate.exception_block_id = effect.target_block_id;
				exception_match = true;
				continue;
			}
			if (match == nullptr) {
				match = &candidate;
			}
		}
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
			if (!exception_match) {
				char message[160];

				std::snprintf(message, sizeof(message),
					"value exception route at source %u has no executable instruction (opcode %u)",
					effect.source_position_id,
					static_cast<unsigned int>(source_opcode));
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					message);
				return false;
			}
			continue;
		}
		if (match == nullptr) {
			continue;
		}
		if (match->debug_probe) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"debug-probe source effect is duplicated");
			return false;
		}
		match->debug_probe = true;
		require_runtime_helper(plan, ZEND_NATIVE_HELPER_SOURCE_PROBE);
	}
	return true;
}

[[nodiscard]]
static bool check_instruction_operands(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	zend_native_diagnostic *const diag = builder.diag;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			if (zend_tpde_value_index(
					plan, zend_tpde_operand_at(plan, &instruction, n)) < 0) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR operand table references an unknown value");
				return false;
			}
		}
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR) {
			zend_mir_value_id expected;
			if (!source_operand_value_id(
					instruction.value_operation.op1, expected)
					|| instruction.operand_count != 1
					|| zend_tpde_operand_at(plan, &instruction, 0) != expected) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"semantic echo MIR operand differs from source semantics");
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]]
static bool freeze_plan_passes(plan_builder &builder) {
	zend_tpde_plan *const plan = builder.plan;
	const zend_mir_view *const view = builder.view;
	const zend_mir_value_view *const value_model = builder.value_model;
	const zend_op_array *const source_op_array = builder.source_op_array;
	const zend_ssa *const source_ssa = builder.source_ssa;
	zend_native_diagnostic *const diag = builder.diag;
	if (!freeze_source_value_bindings(
			plan, source_op_array, source_ssa, diag)) {
		return false;
	}
	freeze_dynamic_fetch_cv_indices(plan, source_op_array, source_ssa);
	if (!freeze_source_call_phases(plan, diag)) {
		return false;
	}
	freeze_machine_control_flow(plan);
	if (!freeze_machine_references(plan, diag)) {
		return false;
	}
	if (!freeze_generator_resume_liveness(
			plan, source_op_array, value_model, diag)) {
		return false;
	}
	if (!freeze_deopt_resume_targets(
			plan, source_op_array, source_ssa, diag)) {
		return false;
	}
	if (!freeze_statepoint_materializations(
			plan, view, source_op_array, diag)) {
		return false;
	}
	if (!freeze_machine_plan_consumers(plan, diag)) {
		return false;
	}
	freeze_register_boolean_results(plan);
	freeze_fused_compare_branches(plan);
	/* After the fusion: a fused operation's helper needs no initialized
	 * destination in the Zend entry. */
	if (!freeze_entry_undef_temporaries(plan, source_op_array, diag)) {
		return false;
	}
	plan->may_emit_calls = false;
	for (uint32_t index = 0;
			index < ZEND_NATIVE_RUNTIME_HELPER_WORD_COUNT; ++index) {
		plan->may_emit_calls = plan->may_emit_calls
			|| plan->required_runtime_helpers[index] != 0;
	}
	return true;
}

bool initialize_plan(
	const zend_mir_view *view,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag) {
	plan_builder builder{view, user_bindings, user_binding_count,
		internal_bindings, internal_binding_count, effects, effect_count,
		frame_argument_count, source_op_array, source_ssa, plan, diag};

	freeze_entry_facts(builder);
	if (!read_mir_function(builder)
			|| !freeze_source_opcodes(builder)
			|| !freeze_compiled_variable_uses(builder)
			|| !freeze_source_cfg(builder)) {
		return false;
	}
	read_view_counts(builder);
	if (!freeze_user_opcode_operations(builder)
			|| !check_source_frame_bounds(builder)
			|| !check_record_counts(builder)
			|| !allocate_plan_tables(builder)
			|| !freeze_instruction_table(builder)
			|| !freeze_call_tables(builder)
			|| !freeze_value_operations(builder)
			|| !freeze_source_zval_returns(builder)
			|| !freeze_block_graph(builder)
			|| !freeze_values(builder)
			|| !freeze_constants(builder)
			|| !freeze_value_facts(builder)) {
		return false;
	}
	freeze_argument_abi(builder);
	freeze_receive_aliases(builder);
	freeze_return_abi(builder);
	freeze_cyclic_blocks(builder);
	collect_phi_and_reference_storages(builder);
	return freeze_storage_alias(builder)
		&& plan_instructions(builder)
		&& freeze_source_effects(builder)
		&& check_instruction_operands(builder)
		&& freeze_plan_passes(builder);
}

} // namespace zend::native::tpde::plan_detail

using namespace zend::native::tpde::plan_detail;

int32_t zend_tpde_value_index(const zend_tpde_plan *plan, zend_mir_value_id id) {
	return id_index_find(plan->value_index, plan->value_index_capacity, id);
}

int32_t zend_tpde_block_index(const zend_tpde_plan *plan, zend_mir_block_id id) {
	return id_index_find(plan->block_index, plan->block_index_capacity, id);
}

uint32_t zend_tpde_block_successor_count(
	const zend_tpde_plan *plan, zend_mir_block_id id) {
	if (plan->block_successor_offsets == nullptr) {
		return 0;
	}
	const int32_t block_index = zend_tpde_block_index(plan, id);
	if (block_index < 0
			|| static_cast<uint32_t>(block_index) >= plan->block_count) {
		return 0;
	}
	const uint32_t index = static_cast<uint32_t>(block_index);
	return plan->block_successor_offsets[index + 1]
		- plan->block_successor_offsets[index];
}

bool zend_tpde_block_successor_at(
	const zend_tpde_plan *plan,
	zend_mir_block_id id,
	uint32_t successor_index,
	zend_mir_block_id *out) {
	if (out == nullptr
			|| plan->block_successor_offsets == nullptr
			|| plan->block_successors == nullptr) {
		return false;
	}
	const int32_t block_index = zend_tpde_block_index(plan, id);
	if (block_index < 0
			|| static_cast<uint32_t>(block_index) >= plan->block_count) {
		return false;
	}
	const uint32_t index = static_cast<uint32_t>(block_index);
	const uint32_t begin = plan->block_successor_offsets[index];
	const uint32_t count =
		plan->block_successor_offsets[index + 1] - begin;
	if (successor_index >= count) {
		return false;
	}
	const uint32_t target_index =
		plan->block_successors[begin + successor_index];
	if (target_index >= plan->block_count || plan->block_ids == nullptr) {
		return false;
	}
	*out = plan->block_ids[target_index];
	return true;
}

int32_t zend_tpde_instruction_index(
	const zend_tpde_plan *plan, zend_mir_instruction_id id) {
	return id_index_find(
		plan->instruction_index, plan->instruction_index_capacity, id);
}

const zend_mir_instruction_record &zend_tpde_instruction_record_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction) {
	(void) plan;
	return instruction->record;
}

bool zend_tpde_call_argument_at(
	const zend_tpde_plan *plan,
	uint32_t index,
	zend_mir_call_argument_ref *out) {
	return plan != nullptr && out != nullptr
		&& index < plan->call_argument_count
		&& plan->call_arguments != nullptr
		&& ((*out = plan->call_arguments[index]), true);
}

zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index) {
	if (instruction == nullptr
			|| index >= instruction->operand_count
			|| instruction->operand_offset > plan->instruction_operand_count
			|| instruction->operand_count
				> plan->instruction_operand_count - instruction->operand_offset
			|| plan->instruction_operands == nullptr) {
		return ZEND_MIR_ID_INVALID;
	}
	return plan->instruction_operands[instruction->operand_offset + index];
}
