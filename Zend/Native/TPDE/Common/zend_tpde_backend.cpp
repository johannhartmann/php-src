// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

static zend_native_image_metrics collect_plan_metrics(
	const zend_tpde_plan &plan) {
	zend_native_image_metrics metrics{};

	for (uint32_t index = 0; index < plan.instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan.instructions[index];
		const zend_native_runtime_helper_id helper =
			instruction.runtime_helper;
		bool guarded_fast_path = false;
		zend_tpde_array_read array_read{};
		zend_tpde_packed_array_append array_append{};
		zend_tpde_array_isset array_isset{};
		zend_tpde_string_length string_length{};
		zend_tpde_string_identity string_identity{};
		zend_tpde_value_condition value_condition{};
		zend_tpde_slot_isset_empty slot_isset{};
		zend_tpde_packed_iterator_fetch packed_iterator_fetch{};
		zend_tpde_array_iterator_reset array_iterator_reset{};
		zend_tpde_object_property_read property_read{};
		zend_tpde_object_property_write property_write{};

		if (helper != ZEND_NATIVE_HELPER_COUNT) {
			metrics.runtime_helper_sites++;
		}
		guarded_fast_path =
			zend_tpde_array_read_at(instruction, &array_read)
			|| zend_tpde_packed_array_append_at(
				instruction, &array_append)
			|| zend_tpde_array_isset_at(instruction, &array_isset)
			|| zend_tpde_string_length_at(instruction, &string_length)
			|| zend_tpde_string_identity_at(
				instruction, &string_identity)
			|| zend_tpde_value_condition_at(
				instruction, &value_condition)
			|| zend_tpde_slot_isset_empty_at(
				instruction, &slot_isset)
			|| zend_tpde_packed_iterator_fetch_at(
				instruction, &packed_iterator_fetch)
			|| zend_tpde_array_iterator_reset_at(
				instruction, &array_iterator_reset)
			|| zend_tpde_object_property_read_at(
				instruction, &property_read)
			|| zend_tpde_object_property_write_at(
				instruction, &property_write);
		if (guarded_fast_path) {
			metrics.guard_sites++;
			if (helper != ZEND_NATIVE_HELPER_COUNT) {
				metrics.slow_path_sites++;
			}
		}
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(&plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& !instruction.user_call_no_call
				&& (instruction.direct_call != nullptr
					|| (instruction.user_call != nullptr
						&& instruction.user_call->do_opcode
							!= ZEND_CALLABLE_CONVERT
						&& instruction.user_call->do_opcode
							!= ZEND_CALLABLE_CONVERT_PARTIAL))) {
			/* Fixed and runtime-resolved user targets both count as direct
			 * Zend-frame call sites, whatever descriptor they carry. */
			metrics.direct_call_sites++;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr) {
			const bool typed_component_call =
				plan.typed_component_call_eligible != nullptr
				&& plan.typed_component_call_eligible[index] != 0;
			const bool effect_closed_inline =
				plan.effect_closed_inline_eligible != nullptr
				&& plan.effect_closed_inline_eligible[index] != 0;
			const bool inline_frame =
				(instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;

			/*
			 * Typed and effect-closed calls and the fast path of an
			 * inline-frame call make no helper call; any other descriptor
			 * calls the frame entry and finish helpers. Stack growth and
			 * exception or interrupt completion are slow paths, not counted.
			 */
			if (!typed_component_call
					&& !effect_closed_inline
					&& !inline_frame) {
				metrics.inner_call_runtime_helper_calls += 2;
			}
		}
	}
	for (uint32_t index = 0; index < plan.direct_call_count; ++index) {
		if (plan.direct_calls[index] != nullptr) {
			metrics.direct_call_frame_bytes +=
				plan.direct_calls[index]->frame_size;
		}
	}
	return metrics;
}

/* Destroys the first count plans. */
static void destroy_component_plans(zend_tpde_plan *plans, uint32_t count) {
	for (uint32_t index = 0; index < count; ++index) {
		destroy_plan(&plans[index]);
	}
}

/*
 * Appends a plan's call descriptors to the image's arrays, which have room
 * for them; the image owns them afterwards.
 */
static void append_call_descriptors(
	zend_native_image *image, zend_tpde_plan *plan) {
	if (plan->direct_call_count != 0) {
		std::memcpy(
			image->direct_calls + image->direct_call_count,
			plan->direct_calls,
			static_cast<size_t>(plan->direct_call_count)
				* sizeof(*image->direct_calls));
	}
	image->direct_call_count += plan->direct_call_count;
	plan->direct_call_count = 0;
	if (plan->direct_internal_call_count != 0) {
		std::memcpy(
			image->direct_internal_calls
				+ image->direct_internal_call_count,
			plan->direct_internal_calls,
			static_cast<size_t>(
				plan->direct_internal_call_count)
				* sizeof(*image->direct_internal_calls));
	}
	image->direct_internal_call_count +=
		plan->direct_internal_call_count;
	plan->direct_internal_call_count = 0;
	if (plan->user_call_count != 0) {
		std::memcpy(
			image->user_calls + image->user_call_count,
			plan->user_calls,
			static_cast<size_t>(plan->user_call_count)
				* sizeof(*image->user_calls));
	}
	image->user_call_count += plan->user_call_count;
	plan->user_call_count = 0;
}

} // namespace zend::native::tpde::plan_detail

using namespace zend::native::tpde::plan_detail;

void zend_tpde_set_diagnostic(
	zend_native_diagnostic *diag,
	zend_native_diagnostic_code code,
	const char *message) {
	if (diag == nullptr) {
		return;
	}
	diag->code = code;
	std::snprintf(diag->message, sizeof(diag->message), "%s", message);
}

extern "C" zend_result zend_tpde_compile_component(
	zend_native_target target,
	const zend_native_component_member *members,
	uint32_t member_count,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	if (diag != nullptr) {
		std::memset(diag, 0, sizeof(*diag));
	}
	if (members == nullptr || member_count == 0
			|| !checked_count(member_count)
			|| out_image == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"component members and out_image are required");
		return FAILURE;
	}
	for (uint32_t index = 0; index < member_count; ++index) {
		const zend_native_component_member &member = members[index];
		if (member.module == nullptr
				|| (member.user_binding_count != 0
					&& member.user_bindings == nullptr)
				|| (member.internal_binding_count != 0
					&& member.internal_bindings == nullptr)
				|| (member.effect_count != 0 && member.effects == nullptr)
				|| !checked_count(member.user_binding_count)
				|| !checked_count(member.internal_binding_count)
				|| !checked_count(member.effect_count)
				|| !checked_count(member.frame_argument_count)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native component member is invalid");
			return FAILURE;
		}
	}
	*out_image = nullptr;
	if (target != ZEND_NATIVE_HOST_TARGET) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_TARGET,
			"only the host target backend is built");
		return FAILURE;
	}

	/* Working storage of this compile (zend_tpde_plan_internal.hpp). */
	std::vector<zend_tpde_plan> component_plans(member_count);
	std::vector<const zend_tpde_plan *> plan_refs(member_count);
	zend_tpde_plan *const plans = component_plans.data();
	uint32_t initialized = 0;
	for (; initialized < member_count; ++initialized) {
		const zend_native_component_member &member = members[initialized];
		/* The deoptimization pairing shapes the plan's resume IDs. */
		plans[initialized].deopt_generic_member_plus_one =
			member.deopt_generic_member_plus_one <= member_count
				? member.deopt_generic_member_plus_one : 0;
		plans[initialized].deopt_landings = member.deopt_landings;
		plans[initialized].call_count_cell = member.call_count_cell;
		memcpy(plans[initialized].tier2_arg_guards, member.tier2_arg_guards,
			sizeof(member.tier2_arg_guards));
		plans[initialized].tier2_fallback_entry = member.tier2_fallback_entry;
		if (!initialize_plan(
				member.module,
				member.user_bindings, member.user_binding_count,
				member.internal_bindings, member.internal_binding_count,
				member.effects, member.effect_count,
				member.frame_argument_count,
				member.source_op_array, member.source_ssa,
				&plans[initialized], diag)) {
			break;
		}
		plans[initialized].symbol_namespace = initialized;
		plans[initialized].entry_variant_member_plus_one =
			member.entry_variant_member_plus_one <= member_count
				? member.entry_variant_member_plus_one : 0;
		plans[initialized].entry_variant_long_mask =
			member.entry_variant_long_mask;
		plans[initialized].entry_variant_numeric =
			member.entry_variant_numeric;
		/* A generic copy is entered only by deoptimization. A member that
		 * runs rarely is entered through the shared fast Do: its own native
		 * call entry would only add bytes. */
		if (plans[initialized].deopt_landings
				|| (zend_tpde_layout_rare_function(
						initialized, &plans[initialized])
					& ZEND_TPDE_RARE_NO_CALL_ENTRY) != 0) {
			plans[initialized].fast_call_eligible = false;
		}
		plan_refs[initialized] = &plans[initialized];
	}
	if (initialized != member_count) {
		destroy_component_plans(plans, initialized + 1);
		return FAILURE;
	}
	if (!freeze_component_machine_plan(
			plans, plan_refs.data(), member_count, diag)) {
		destroy_component_plans(plans, member_count);
		return FAILURE;
	}
	zend_native_image *image = static_cast<zend_native_image *>(
		std::calloc(1, sizeof(*image)));
	if (image == nullptr) {
		destroy_component_plans(plans, member_count);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a native image");
		return FAILURE;
	}
	image->target = target;
	image->build_id = native_image_build_id(target);
	image->argument_count = plans[0].argument_count;
	image->frame_variable_count = members[0].source_op_array != nullptr
		? static_cast<uint32_t>(members[0].source_op_array->last_var)
		: plans[0].argument_count;
	image->frame_temporary_count =
		members[0].source_op_array != nullptr
			? members[0].source_op_array->T : 0;
	image->component_entries = static_cast<zend_native_component_entry *>(
		std::calloc(member_count, sizeof(*image->component_entries)));
	if (image->component_entries == nullptr) {
		destroy_component_plans(plans, member_count);
		zend_native_image_destroy(image);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native component entry metadata");
		return FAILURE;
	}
	image->component_entry_count = member_count;
	for (uint32_t index = 0; index < member_count; ++index) {
		image->component_entries[index].argument_count =
			plans[index].argument_count;
		image->component_entries[index].frame_variable_count =
			members[index].source_op_array != nullptr
				? static_cast<uint32_t>(
					members[index].source_op_array->last_var)
				: plans[index].argument_count;
		image->component_entries[index].frame_temporary_count =
			members[index].source_op_array != nullptr
				? members[index].source_op_array->T : 0;
	}
	bool symbols_ready = true;
	for (uint32_t index = 0; index < member_count; ++index) {
		/* Metrics count source sites: a specialized function's generic
		 * copy repeats them. */
		const zend_native_image_metrics metrics =
			plans[index].deopt_landings
				? zend_native_image_metrics{}
				: collect_plan_metrics(plans[index]);
		image->metrics.runtime_helper_sites += metrics.runtime_helper_sites;
		image->metrics.guard_sites += metrics.guard_sites;
		image->metrics.slow_path_sites += metrics.slow_path_sites;
		image->metrics.direct_call_sites += metrics.direct_call_sites;
		image->metrics.direct_call_frame_bytes +=
			metrics.direct_call_frame_bytes;
		image->metrics.inner_call_runtime_helper_calls +=
			metrics.inner_call_runtime_helper_calls;
		if (!prepare_image_symbols(&plans[index], image, diag)) {
			symbols_ready = false;
			break;
		}
	}
	zend_result result = symbols_ready
		? zend_tpde_emit_host(plan_refs.data(), member_count, image, diag)
		: FAILURE;
	if (result == SUCCESS) {
		uint32_t direct_count = 0;
		uint32_t direct_internal_count = 0;
		uint32_t user_count = 0;
		for (uint32_t index = 0; index < member_count; ++index) {
			if (plans[index].direct_call_count > MAX_RECORDS - direct_count
					|| plans[index].direct_internal_call_count
						> MAX_RECORDS - direct_internal_count
					|| plans[index].user_call_count
						> MAX_RECORDS - user_count) {
				result = FAILURE;
				break;
			}
			direct_count += plans[index].direct_call_count;
			direct_internal_count += plans[index].direct_internal_call_count;
			user_count += plans[index].user_call_count;
		}
		if (result == SUCCESS) {
			image->direct_calls = direct_count == 0 ? nullptr
				: static_cast<zend_native_direct_call_descriptor **>(
					std::calloc(direct_count, sizeof(*image->direct_calls)));
			image->direct_internal_calls =
				direct_internal_count == 0 ? nullptr
				: static_cast<zend_native_direct_internal_call_descriptor **>(
					std::calloc(direct_internal_count,
						sizeof(*image->direct_internal_calls)));
			image->user_calls = user_count == 0 ? nullptr
				: static_cast<zend_native_user_call_descriptor **>(
					std::calloc(user_count, sizeof(*image->user_calls)));
			if ((direct_count != 0 && image->direct_calls == nullptr)
					|| (direct_internal_count != 0
						&& image->direct_internal_calls == nullptr)
					|| (user_count != 0 && image->user_calls == nullptr)) {
				result = FAILURE;
			}
		}
		if (result == SUCCESS) {
			for (uint32_t index = 0; index < member_count; ++index) {
				append_call_descriptors(image, &plans[index]);
			}
		}
	}
	destroy_component_plans(plans, member_count);
	if (result == FAILURE) {
		if (diag != nullptr && diag->code == ZEND_NATIVE_DIAGNOSTIC_OK) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to retain native component metadata");
		}
		zend_native_image_destroy(image);
		return FAILURE;
	}
	*out_image = image;
	return SUCCESS;
}
