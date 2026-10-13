// SPDX-License-Identifier: PHP-3.01
#pragma once

/*
 * Declarations shared by the translation units that build a TPDE plan and
 * its native image (zend_tpde_backend.cpp and its zend_tpde_plan*.cpp,
 * zend_tpde_machine_plan.cpp and zend_tpde_image.cpp siblings). Backends
 * use zend_tpde_internal.hpp instead.
 */

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_system_id.h"
#include "Zend/zend_type_info.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/Optimizer/zend_func_info.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string_view>

namespace zend::native::tpde::plan_detail {

constexpr uint32_t MAX_RECORDS = UINT32_C(1) << 20;

/* Native image symbols (zend_tpde_image.cpp). */
uint64_t native_image_build_id(zend_native_target target);
bool prepare_image_symbols(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);

/* Source and type analysis (zend_tpde_plan_analysis.cpp). */
bool source_descriptor_operand(
	const zend_op_array *op_array,
	const zend_op *opline,
	uint8_t operand_type,
	const znode_op &node,
	zend_mir_source_operand_ref *out);
zend_mir_storage_id source_descriptor_storage(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref &operand);
bool user_opcode_source_operation(
	const zend_op_array *op_array, uint32_t source_position,
	zend_mir_executable_value_ref *operation);
uint32_t user_opcode_source_absolute_target(
	const zend_op_array *op_array, uint32_t source_position,
	const znode_op &operand);
uint32_t user_opcode_source_extended_target(
	const zend_op_array *op_array, uint32_t source_position,
	uint32_t extended_value);
bool user_opcode_target(
	uint32_t opcode, zend_tpde_user_opcode_target *target);
bool source_descriptor_send_opcode(uint8_t opcode);
bool exact_scalar_satisfies_type(
	zend_mir_scalar_type_mask exact_type, const zend_type &type);
bool literal_satisfies_type(const zval *literal, const zend_type &type);
zend_mir_scalar_type_mask exact_scalar_from_declared_type(
	const zend_type &type);
zend_tpde_local_abi_type zend_tpde_local_abi_from_declared_type(
	const zend_type *type, bool by_reference,
	zend_tpde_local_abi_transfer transfer);
void zend_tpde_apply_machine_value_facts(
	zend_tpde_value *value,
	bool register_definition);
bool zend_tpde_ssa_variable_rebindable(
	const zend_op_array *op_array, const zend_ssa *ssa, uint32_t variable);
void zend_tpde_refine_non_alias_scalar_values(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	const std::vector<uint8_t> &register_definitions);
void zend_tpde_refine_literal_assignment_values(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa);

/*
 * A fixed point over instructions in index order, pass after pass, like a
 * full sweep until nothing changes. evaluate(index) returns the index of the
 * value it changed, or -1; an instruction is evaluated again only after
 * visit_users reported it as a user of a changed value. Its evaluation
 * reads only its own operands and its result, so a skipped evaluation would
 * have changed nothing.
 */
template <typename Evaluate, typename VisitUsers>
void sweep_to_fixed_point(
		uint32_t instruction_count, Evaluate &&evaluate,
		VisitUsers &&visit_users) {
	std::vector<uint8_t> dirty(instruction_count, 1);
	for (bool swept = true; swept;) {
		swept = false;
		for (uint32_t index = 0; index < instruction_count; ++index) {
			if (dirty[index] == 0) {
				continue;
			}
			dirty[index] = 0;
			const int32_t changed = evaluate(index);
			if (changed < 0) {
				continue;
			}
			swept = true;
			visit_users(static_cast<uint32_t>(changed),
				[&](uint32_t user) {
					if (user < instruction_count) {
						dirty[user] = 1;
					}
				});
		}
	}
}

/* Visits the instructions that read a value as a MIR operand. */
void visit_operand_users(const zend_tpde_plan *plan,
		uint32_t value_index, const auto &visit) {
	if (plan->value_consumer_offsets == nullptr
			|| value_index >= plan->value_count) {
		return;
	}
	for (uint32_t use = plan->value_consumer_offsets[value_index];
			use < plan->value_consumer_offsets[value_index + 1]; ++use) {
		const zend_tpde_machine_use &consumer = plan->value_consumers[use];
		if (consumer.kind == ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND
				|| consumer.kind == ZEND_TPDE_MACHINE_USE_PHI_EDGE) {
			visit(consumer.instruction_index);
		}
	}
}

void zend_tpde_refine_boxed_scalar_copies(zend_tpde_plan *plan);
zend_mir_scalar_type_mask exact_scalar_from_zval(const zval *value);
uint64_t scalar_bits_from_zval(const zval *value);

/*
 * Answers of exact_scalar_from_ssa_value per SSA variable. The search gives
 * up below depth 64, so an answer is kept only when its search stayed above
 * that limit, with the depth it reached below its variable (its height); it
 * stands for a search from any depth that leaves room for that height.
 */
struct exact_scalar_ssa_memo {
	std::vector<zend_mir_scalar_type_mask> types;
	/* 0 when unknown, else 1 + the height. */
	std::vector<uint32_t> heights;
};

bool source_argument_is_received_parameter(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument);
zend_mir_scalar_type_mask exact_scalar_from_source_argument(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_mir_call_argument_ref &argument,
	exact_scalar_ssa_memo *memo);
bool source_call_argument_may_be_undefined(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument);
bool source_call_argument_stable_after_send(
	const zend_op_array *op_array,
	const zend_mir_call_argument_ref &argument,
	uint32_t call_source_position);
bool direct_call_parameter_ordinal(
	const zend_op_array *source_op_array,
	const zend_function *callee,
	const zend_mir_call_argument_ref &argument,
	uint32_t *parameter_ordinal);
bool source_operand_value_id(
	const zend_mir_source_operand_ref &operand, zend_mir_value_id &value_id);
zend_mir_scalar_type_mask source_operand_exact_type(
	const zend_tpde_plan *plan,
	const zend_mir_source_operand_ref &operand);
zend_native_runtime_helper_id executable_value_helper(zend_mir_opcode opcode);

/* Call-site planning (zend_tpde_plan_calls.cpp). */
bool call_site_requires_source_fragments(
	const zend_tpde_plan *plan, const zend_mir_call_site_ref &site);
bool call_site_participates_in_nested_call(
	const zend_mir_call_view *calls, const zend_mir_call_site_ref &site);
bool nested_call_site_direct_candidate(
	const zend_tpde_plan *plan, const zend_mir_call_view *calls,
	const zend_native_call_binding *user_bindings,
	const zend_native_internal_call_binding *internal_bindings,
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_call_site_ref &site);
zend_native_user_call_descriptor *build_user_call_descriptor(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_call_site_ref &site,
	const zend_mir_instruction_record &record,
	zend_native_diagnostic *diag);
bool freeze_source_call_phases(
	zend_tpde_plan *plan, zend_native_diagnostic *diag);

/* Machine consumers, branch fusion and control-flow flags
 * (zend_tpde_plan_consumers.cpp). */
uint32_t generator_suspend_value_index(
	const zend_tpde_plan *plan, const int32_t *definitions,
	uint32_t suspend_instruction, uint32_t value_index);
bool freeze_machine_plan_consumers(
	zend_tpde_plan *plan, zend_native_diagnostic *diag);
void freeze_fused_compare_branches(zend_tpde_plan *plan);
void freeze_register_boolean_results(zend_tpde_plan *plan);
void freeze_machine_control_flow(zend_tpde_plan *plan);

/* Source value bindings, references and frame liveness
 * (zend_tpde_plan_bindings.cpp). */
bool freeze_source_value_bindings(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag);
void freeze_dynamic_fetch_cv_indices(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa);
bool freeze_machine_references(
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag);
bool freeze_generator_resume_liveness(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_value_view *value_model,
	zend_native_diagnostic *diag);
bool freeze_entry_undef_temporaries(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag);

/* Deoptimization resume targets and statepoint materializations
 * (zend_tpde_plan_deopt.cpp). */
bool freeze_deopt_resume_targets(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag);
bool freeze_statepoint_materializations(
	zend_tpde_plan *plan,
	const zend_mir_view *view,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag);
bool freeze_deopt_exits(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag);
/* Frees the generic copy's landing adaptor freeze_deopt_exits() built. */
void destroy_deopt_landing_adaptor(zend_tpde_plan *plan);

/* Plan construction and lookup indices (zend_tpde_plan.cpp). */
bool checked_count(uint32_t count);
uint32_t id_index_capacity(uint32_t count);
bool id_index_insert(
	zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id,
	uint32_t index);
int32_t id_index_find(
	const zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id);
void require_runtime_helper(
	zend_tpde_plan *plan, zend_native_runtime_helper_id helper);
void destroy_plan(zend_tpde_plan *plan);
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
	zend_native_diagnostic *diag);

/* Component machine plan (zend_tpde_machine_plan.cpp). */
bool machine_plan_type_check_supported(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction);
void destroy_machine_cfg(zend_tpde_machine_cfg *cfg);
bool freeze_component_machine_plan(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag);

} // namespace zend::native::tpde::plan_detail
