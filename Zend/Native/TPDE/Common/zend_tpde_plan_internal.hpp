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
[[nodiscard]] bool prepare_image_symbols(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);

/*
 * Frees the direct, direct internal and user call descriptors a plan, an
 * image or native code owns, with their arrays.
 */
template <typename Owner>
void free_call_descriptors(Owner *owner) {
	for (uint32_t index = 0; index < owner->direct_call_count; ++index) {
		std::free(owner->direct_calls[index]);
	}
	std::free(owner->direct_calls);
	for (uint32_t index = 0;
			index < owner->direct_internal_call_count; ++index) {
		std::free(owner->direct_internal_calls[index]);
	}
	std::free(owner->direct_internal_calls);
	for (uint32_t index = 0; index < owner->user_call_count; ++index) {
		zend_native_user_call_descriptor_free(owner->user_calls[index]);
	}
	std::free(owner->user_calls);
}

/* Frees the internal call cells an image or native code owns. */
template <typename Owner>
void free_internal_call_cells(Owner *owner) {
	for (uint32_t index = 0;
			index < owner->owned_internal_call_cell_count; ++index) {
		std::free(owner->owned_internal_call_cells[index]);
	}
	std::free(owner->owned_internal_call_cells);
}

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

/*
 * What initialize_plan()'s phases share while they build one member's plan:
 * the member's inputs and what an earlier phase derived for a later one.
 */
struct plan_builder {
	const zend_mir_view *view;
	const zend_native_call_binding *user_bindings;
	uint32_t user_binding_count;
	const zend_native_internal_call_binding *internal_bindings;
	uint32_t internal_binding_count;
	const zend_native_source_effect *effects;
	uint32_t effect_count;
	uint32_t frame_argument_count;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;
	zend_tpde_plan *plan;
	zend_native_diagnostic *diag;

	exact_scalar_ssa_memo exact_scalar_memo{};
	const zend_mir_call_view *calls = nullptr;
	const zend_mir_value_view *value_model = nullptr;
	uint32_t constant_count = 0;
	uint32_t frame_slot_count = 0;
	/* Per block: 1 when it lies on a cycle of the CFG. */
	std::vector<uint8_t> cyclic_blocks;
	/* Sorted: the storages a PHI defines, and those ASSIGN_REF binds. */
	std::vector<zend_mir_storage_id> phi_storages;
	std::vector<zend_mir_storage_id> reference_assigned_storages;
	/* Per CV: 1 when its scalar stores stay eager (freeze_storage_alias()). */
	std::vector<uint8_t> storage_may_alias;
	bool lazy_capable_function = false;
	/*
	 * The call sites as plan->call_sites holds them, read once from the view
	 * in this order. Unless two sites share an ID, a site's own index is
	 * the one of its ID, and whether another site nests in it or it in
	 * another follows from the sites ordered by INIT.
	 */
	const zend_mir_call_site_ref *sites = nullptr;
	uint32_t site_count = 0;
	/* 1 when the sites' IDs are unique, 0 when not, -1 before the check. */
	int8_t unique_site_ids = -1;
	/* Per call site: 1 when it nests in another site or another in it. */
	std::vector<uint8_t> nested_participants;
	/*
	 * Per call site: 1 when its group of overlapping calls may use direct
	 * calls, 0 when it may not, -1 before the group is examined.
	 */
	std::vector<int8_t> nested_direct_groups;
	/* The frame states by ID, read at the first interrupt statepoint. */
	bool interrupt_frames_read = false;
	std::vector<zend_mir_frame_state_ref> interrupt_frames;
	std::vector<zend_tpde_id_index_entry> interrupt_frame_index;
	uint32_t interrupt_frame_index_capacity = 0;
};

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
[[nodiscard]] zend_native_user_call_descriptor *build_user_call_descriptor(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_call_site_ref &site,
	const zend_mir_instruction_record &record,
	zend_native_diagnostic *diag);
[[nodiscard]] bool freeze_source_call_phases(
	zend_tpde_plan *plan, zend_native_diagnostic *diag);
[[nodiscard]] bool plan_call_site(plan_builder &builder, uint32_t i);

/* Machine consumers, branch fusion and control-flow flags
 * (zend_tpde_plan_consumers.cpp). */
uint32_t generator_suspend_value_index(
	const zend_tpde_plan *plan, const int32_t *definitions,
	uint32_t suspend_instruction, uint32_t value_index);
[[nodiscard]] bool freeze_machine_plan_consumers(
	zend_tpde_plan *plan, zend_native_diagnostic *diag);
void freeze_fused_compare_branches(zend_tpde_plan *plan);
void freeze_fused_container_reads(zend_tpde_plan *plan);
void freeze_register_boolean_results(zend_tpde_plan *plan);
void freeze_machine_control_flow(zend_tpde_plan *plan);

/* Source value bindings, references and frame liveness
 * (zend_tpde_plan_bindings.cpp). */
[[nodiscard]] bool freeze_source_value_bindings(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag);
void freeze_dynamic_fetch_cv_indices(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa);
[[nodiscard]] bool freeze_machine_references(
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag);
[[nodiscard]] bool freeze_generator_resume_liveness(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_value_view *value_model,
	zend_native_diagnostic *diag);
[[nodiscard]] bool freeze_entry_undef_temporaries(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag);

/* Deoptimization resume targets and statepoint materializations
 * (zend_tpde_plan_deopt.cpp). */
[[nodiscard]] bool freeze_deopt_resume_targets(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag);
[[nodiscard]] bool freeze_statepoint_materializations(
	zend_tpde_plan *plan,
	const zend_mir_view *view,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag);
[[nodiscard]] bool freeze_deopt_exits(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag);
/* Frees the generic copy's landing adaptor freeze_deopt_exits() built. */
void destroy_deopt_landing_adaptor(zend_tpde_plan *plan);

/* Plan construction and lookup indices (zend_tpde_plan.cpp). */

/*
 * Allocation failure. An array that a plan, an image or native code owns
 * outlives the function that builds it and is freed by its owner's destroy
 * function: it is allocated with std::malloc or std::calloc, mostly into a
 * zend_tpde_owned, and a failure returns an ALLOCATION_FAILED diagnostic.
 * Working storage that is gone when the building function returns is a
 * std::vector or another standard container, not a C allocation; its sizes
 * stay within the record bounds (checked_count, MAX_RECORDS), and its
 * allocation failure terminates the process.
 */

/*
 * Allocates count zeroed elements into out; false, with an allocation
 * diagnostic of the given message, when that fails.
 */
template <typename T>
[[nodiscard]] bool plan_alloc(zend_tpde_owned<T> &out, size_t count,
	zend_native_diagnostic *diag, const char *message) {
	out.reset(static_cast<T *>(std::calloc(count, sizeof(T))));
	if (out == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED, message);
		return false;
	}
	return true;
}

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
/* The generator resume of a target source position, or UINT32_MAX; the
 * targets are sorted and unique. */
inline uint32_t generator_resume_at_target(
	const zend_tpde_plan *plan, uint32_t target) {
	if (plan->generator_resume_targets == nullptr) {
		return UINT32_MAX;
	}
	const uint32_t *begin = plan->generator_resume_targets;
	const uint32_t *end = begin + plan->generator_resume_count;
	const uint32_t *found = std::lower_bound(begin, end, target);
	return found != end && *found == target
		? static_cast<uint32_t>(found - begin) : UINT32_MAX;
}

void require_runtime_helper(
	zend_tpde_plan *plan, zend_native_runtime_helper_id helper);
void destroy_plan(zend_tpde_plan *plan);
[[nodiscard]] bool initialize_plan(
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
[[nodiscard]] bool freeze_component_machine_plan(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag);

} // namespace zend::native::tpde::plan_detail
