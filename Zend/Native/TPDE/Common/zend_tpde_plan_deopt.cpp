// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"

namespace zend::native::tpde::plan_detail {

/* Deoptimization stress (debug builds, ZEND_NATIVE_DEOPT_STRESS=<period>):
 * every non-generator function gets resume IDs, so the PHPT tiers exercise
 * each guarded operation's transfer. */
static bool deopt_stress_enabled() {
#if ZEND_DEBUG
	static const bool enabled = [] {
		const char *setting = std::getenv("ZEND_NATIVE_DEOPT_STRESS");
		return setting != nullptr && std::strtol(setting, nullptr, 10) > 0;
	}();
	return enabled;
#else
	return false;
#endif
}

/* An operation a specialized version may guard, outside every pending
 * call: the transfer moves frame slots, not the native state of a call
 * under construction. */
static bool deopt_resume_target(const zend_op_array *op_array,
		const zend_ssa *ssa, uint32_t position, uint32_t call_depth) {
	if (call_depth != 0 || position >= op_array->last
			|| ssa->cfg.map == nullptr) {
		return false;
	}
	const int block = ssa->cfg.map[position];
	if (block < 0 || block >= ssa->cfg.blocks_count
			|| (ssa->cfg.blocks[block].flags & ZEND_BB_REACHABLE) == 0) {
		return false;
	}
	switch (op_array->opcodes[position].opcode) {
		case ZEND_FETCH_DIM_R:
		case ZEND_FETCH_DIM_IS:
		case ZEND_FETCH_OBJ_R:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_ASSIGN_OP:
			return true;
		default:
			return false;
	}
}

/* The pending-call depth after the opcode at `position`; call sequences
 * nest linearly in opline order. */
static uint32_t deopt_call_depth_after(const zend_op_array *op_array,
		uint32_t position, uint32_t call_depth) {
	switch (op_array->opcodes[position].opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			return call_depth + 1;
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return call_depth != 0 ? call_depth - 1 : 0;
		default:
			return call_depth;
	}
}

/*
 * Deoptimization resume IDs. The generic copy of a specialized function
 * (deopt_landings) has one at every guarded operation outside a pending
 * call, where the specialized version's failed guard resumes. Under
 * deoptimization stress every guardable operation of the other functions is
 * one: a transfer point in its cold path materializes the frame and a new
 * activation resumes before it. The operation must start at a helper-backed
 * MIR instruction, an observable boundary whose statepoint materializations
 * publish the frame. The adaptor derives each landing's frame-state map
 * from machine liveness.
 */
bool freeze_deopt_resume_targets(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag)
{
	const bool stress = deopt_stress_enabled()
		&& plan->deopt_generic_member_plus_one == 0;
	if (source_op_array == nullptr || source_ssa == nullptr
			|| (source_op_array->fn_flags & ZEND_ACC_GENERATOR) != 0
			|| plan->generator_resume_count != 0
			|| plan->user_opcode_callbacks
			|| !zend_tpde_linux_inline_forms
			|| (!stress && !plan->deopt_landings)) {
		return true;
	}
	std::vector<uint32_t> first_instruction(
		source_op_array->last, UINT32_MAX);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const uint32_t position =
			plan->instructions[index].record.source_position_id;
		if (position < source_op_array->last
				&& first_instruction[position] == UINT32_MAX) {
			first_instruction[position] = index;
		}
	}
	std::vector<uint32_t> targets;
	std::vector<uint32_t> instructions;
	uint32_t call_depth = 0;
	for (uint32_t position = 0; position < source_op_array->last;
			++position) {
		const uint32_t index = first_instruction[position];
		const bool guarded = index != UINT32_MAX
			&& (plan->instructions[index].machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) != 0;
		if ((plan->deopt_landings
					? guarded && call_depth == 0
					: deopt_resume_target(source_op_array, source_ssa,
						position, call_depth))
				&& index != UINT32_MAX
				&& plan->instructions[index].has_value_operation
				&& plan->instructions[index].runtime_helper
					!= ZEND_NATIVE_HELPER_COUNT) {
			targets.push_back(position);
			instructions.push_back(index);
		}
		call_depth = deopt_call_depth_after(
			source_op_array, position, call_depth);
	}
	if (targets.empty()) {
		return true;
	}
	if (!checked_count(static_cast<uint32_t>(targets.size()))) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"deoptimization target count exceeds the executable bound");
		return false;
	}
	plan->deopt_resume_count = static_cast<uint32_t>(targets.size());
	plan->deopt_resume_targets.reset(static_cast<uint32_t *>(
		std::malloc(targets.size() * sizeof(uint32_t))));
	plan->deopt_resume_instructions.reset(static_cast<uint32_t *>(
		std::malloc(targets.size() * sizeof(uint32_t))));
	if (plan->deopt_resume_targets == nullptr
			|| plan->deopt_resume_instructions == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze deoptimization targets");
		return false;
	}
	plan->deopt_stress = stress;
	if (stress) {
		require_runtime_helper(plan, ZEND_NATIVE_HELPER_DEOPT_STRESS_REENTER);
	}
	std::copy(targets.begin(), targets.end(),
		plan->deopt_resume_targets.get());
	std::copy(instructions.begin(), instructions.end(),
		plan->deopt_resume_instructions.get());
	return true;
}

namespace {

/*
 * The register value that owns a canonical scalar slot at an observable
 * boundary: a sparse reaching-definition query over the frozen CFG.
 * Exact scalar PHIs end loop queries at their merge point.
 */
struct reaching_scalar_definition {
	int32_t value_index;
	int32_t instruction_index;

	bool operator==(const reaching_scalar_definition &other) const {
		return value_index == other.value_index
			&& instruction_index == other.instruction_index;
	}
};
constexpr reaching_scalar_definition no_reaching_scalar{-1, -1};
constexpr reaching_scalar_definition cyclic_reaching_scalar{-2, -2};

/*
 * The materializations of a plan's observable boundaries: the values a
 * helper, call, statepoint, return or probe needs published to the Zend
 * frame, and the lazy CVs an inline form reads from its slot.
 */
class statepoint_materializer {
public:
	statepoint_materializer(zend_tpde_plan *plan, const zend_mir_view *view,
			const zend_op_array *source_op_array, zend_native_diagnostic *diag)
		: plan(plan), view(view), source_op_array(source_op_array),
			diag(diag) {}

	[[nodiscard]] bool freeze();

private:
	void collect_generator_definitions();
	void collect_block_ranges_and_lazy_scalars();
	reaching_scalar_definition resolve_reaching_scalar(
		uint32_t block_index, uint32_t before, zend_mir_storage_id storage_id);
	reaching_scalar_definition reaching_scalar(
		uint32_t boundary_index, zend_mir_storage_id storage_id);
	[[nodiscard]] bool read_frame_states();
	int32_t definition_value(const zend_tpde_instruction &instruction) const;
	int32_t plain_store_value(const zend_tpde_instruction &instruction) const;
	void index_definitions_and_plain_stores();
	void size_boundary_storage_tables();
	bool instruction_reads_storage(
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record,
		zend_mir_storage_id storage_id);
	void add_read_storage(
		std::vector<zend_mir_storage_id> &read_storages,
		zend_mir_storage_id storage_id) const;
	void collect_lazy_reads(
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record,
		std::vector<zend_mir_storage_id> &read_storages) const;
	uint32_t boundary_materialization(zend_mir_storage_id storage_id) const;
	bool duplicate_storage(zend_mir_storage_id storage_id) const;
	zend_tpde_machine_value_kind defined_machine_kind(
		const zend_tpde_source_value_binding &binding) const;
	bool stored_after_definition(
		zend_mir_storage_id storage_id,
		int32_t source_value_index,
		int32_t source_definition_instruction_index) const;
	void append_materialization(
		uint32_t value_index,
		zend_mir_storage_id storage_id,
		zend_tpde_machine_value_kind machine_kind,
		int32_t source_value_index,
		int32_t source_definition_instruction_index);
	void append_source_materialization(
		zend_mir_storage_id storage_id,
		zend_tpde_machine_value_kind machine_kind,
		int32_t source_value_index,
		int32_t source_definition_instruction_index);
	int32_t source_scalar_definition(zend_mir_storage_id storage_id) const;
	[[nodiscard]] bool materialize_frame_state(
		const zend_mir_instruction_record &record);
	void append_reaching_scalar(zend_mir_storage_id storage_id);
	void materialize_lazy_scalars(const zend_mir_instruction_record &record);
	void materialize_generator_suspend(
		const zend_mir_instruction_record &record);
	[[nodiscard]] bool materialize_helper_inputs(
		const zend_mir_instruction_record &record);
	[[nodiscard]] bool materialize_call_arguments(
		const zend_mir_instruction_record &record);
	[[nodiscard]] bool materialize_boundary(uint32_t index);
	[[nodiscard]] bool publish();

	zend_tpde_plan *plan;
	const zend_mir_view *view;
	const zend_op_array *source_op_array;
	zend_native_diagnostic *diag;
	uint32_t frame_count = 0;
	std::vector<zend_mir_frame_state_ref> frames;
	std::vector<zend_tpde_materialization> materializations;
	std::vector<zend_mir_storage_id> lazy_scalar_storages;
	std::vector<uint32_t> block_instruction_begin;
	std::vector<uint32_t> block_instruction_end;
	/* Value definitions for generator suspensions; the machine-plan table is
	 * frozen only after the materializations. */
	std::vector<int32_t> generator_definitions;
	/* Scratch for every reaching_scalar() query: visiting is clear again
	 * after each one, a block's memo is valid for the query whose epoch it
	 * carries. */
	std::vector<uint8_t> visiting;
	std::vector<uint32_t> memoized;
	std::vector<reaching_scalar_definition> memo;
	uint32_t reaching_epoch = 0;
	uint32_t frame_index_capacity = 0;
	std::vector<zend_tpde_id_index_entry> frame_index;
	/*
	 * Per value, in instruction order: the instructions defining it and the
	 * plain ZVAL_STOREs of it, for stored_after_definition().
	 */
	std::vector<uint32_t> value_definition_offsets;
	std::vector<uint32_t> plain_store_offsets;
	std::vector<uint32_t> value_definitions;
	std::vector<uint32_t> plain_stores;
	/*
	 * Per storage: the materialization the boundary being planned has for it,
	 * valid while the stamp is the boundary's. Larger storage ids are looked
	 * up by a scan of the boundary's materializations.
	 */
	std::vector<uint32_t> boundary_storage_stamps;
	std::vector<uint32_t> boundary_storage_materializations;
	uint32_t boundary_stamp = 0;
	bool operand_observed_plan = false;
	/*
	 * The storages a boundary instruction reads, gathered once per boundary;
	 * all is set when one of them cannot be determined.
	 */
	uint32_t read_storages_stamp = 0;
	bool reads_all_storages = false;
	std::vector<zend_mir_storage_id> boundary_read_storages;
	/* The instruction materialize_boundary() plans. */
	uint32_t current_boundary = 0;
};

bool statepoint_materializer::freeze() {
	frame_count = view->frame_state_count(view->context);
	frames.resize(frame_count);
	block_instruction_begin.assign(
		plan->block_count, plan->instruction_count);
	block_instruction_end.assign(plan->block_count, 0);
	collect_generator_definitions();
	collect_block_ranges_and_lazy_scalars();
	visiting.assign(plan->block_count, 0);
	memoized.assign(plan->block_count, 0);
	memo.assign(plan->block_count, no_reaching_scalar);
	if (!read_frame_states()) {
		return false;
	}
	index_definitions_and_plain_stores();
	size_boundary_storage_tables();
	operand_observed_plan = !ZEND_OBSERVER_ENABLED
		&& plan->operand_observed_storages != nullptr
		&& !plan->deopt_landings && !plan->deopt_stress
		&& plan->deopt_generic_member_plus_one == 0
		&& plan->tier2_fallback_entry == nullptr;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		if (!materialize_boundary(index)) {
			return false;
		}
	}
	return publish();
}

void statepoint_materializer::collect_generator_definitions() {
	if (plan->generator_resume_count != 0) {
		generator_definitions.assign(plan->value_count, -1);
		for (uint32_t index = 0; index < plan->instruction_count; ++index) {
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(
					plan, &plan->instructions[index]);
			const int32_t value_index = zend_mir_id_is_valid(record.result_id)
				? zend_tpde_value_index(plan, record.result_id) : -1;
			if (value_index >= 0) {
				generator_definitions[static_cast<uint32_t>(value_index)] =
					static_cast<int32_t>(index);
			}
		}
	}
}

void statepoint_materializer::collect_block_ranges_and_lazy_scalars() {
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const int32_t block_index = zend_tpde_block_index(
			plan, instruction.record.block_id);
		if (block_index >= 0) {
			const uint32_t block = static_cast<uint32_t>(block_index);
			block_instruction_begin[block] =
				std::min(block_instruction_begin[block], index);
			block_instruction_end[block] =
				std::max(block_instruction_end[block], index + 1);
		}
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
}

reaching_scalar_definition statepoint_materializer::resolve_reaching_scalar(
	uint32_t block_index, uint32_t before, zend_mir_storage_id storage_id) {
	if (block_index >= plan->block_count) {
		return no_reaching_scalar;
	}
	const bool whole_block = before == plan->instruction_count;
	if (whole_block && memoized[block_index] == reaching_epoch) {
		return memo[block_index];
	}
	if (visiting[block_index] != 0) {
		return cyclic_reaching_scalar;
	}
	const uint32_t begin = block_instruction_begin[block_index];
	const uint32_t end = std::min(
		before, block_instruction_end[block_index]);
	for (uint32_t cursor = end; cursor > begin; --cursor) {
		const uint32_t instruction_index = cursor - 1;
		const zend_tpde_instruction &candidate =
			plan->instructions[instruction_index];
		if (candidate.record.block_id
				!= plan->block_ids[block_index]) {
			continue;
		}
		if (candidate.record.opcode
					== ZEND_MIR_OPCODE_ZVAL_STORE
				&& candidate.zval_store_storage_id
					== storage_id
				&& candidate.zval_store_lazy_scalar
				&& candidate.operand_count >= 1) {
			const reaching_scalar_definition found{
				zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &candidate, 0)),
				-1,
			};
			if (whole_block) {
				memo[block_index] = found;
				memoized[block_index] = reaching_epoch;
			}
			return found;
		}
		/* A non-lazy store or another writer leaves the slot
		 * current. */
		if (candidate.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& candidate.zval_store_storage_id == storage_id
				&& !candidate.zval_store_lazy_scalar) {
			return no_reaching_scalar;
		}
		if (candidate.has_value_operation
				&& candidate.record.opcode
					!= ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& candidate.record.opcode
					!= ZEND_MIR_OPCODE_VALUE_INCDEC
				&& candidate.value_operation.op1_storage_id
					== storage_id
				&& candidate.value_operation
					.op1_definition_ssa_variable_id_plus_one != 0) {
			return no_reaching_scalar;
		}
		if (candidate.has_value_operation
				&& candidate.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ASSIGN_REF
				&& (candidate.value_operation.op1_storage_id
						== storage_id
					|| candidate.value_operation.op2_storage_id
						== storage_id)) {
			return no_reaching_scalar;
		}
		if (candidate.has_value_operation
				&& (candidate.record.opcode
						== ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
					|| candidate.record.opcode
						== ZEND_MIR_OPCODE_VALUE_INCDEC)
				&& candidate.value_operation.op1_storage_id
					== storage_id) {
			if (!candidate.mutation_lazy_scalar
					|| candidate.mutation_storage_id != storage_id
					|| candidate.value_operation
						.op1_definition_ssa_variable_id_plus_one == 0) {
				return no_reaching_scalar;
			}
			const reaching_scalar_definition found{
				zend_tpde_value_index(
					plan, zend_mir_value_from_original_ssa(
						candidate.value_operation
							.op1_definition_ssa_variable_id_plus_one
							- 1)),
				static_cast<int32_t>(instruction_index),
			};
			if (whole_block) {
				memo[block_index] = found;
				memoized[block_index] = reaching_epoch;
			}
			return found;
		}
		if (!zend_mir_id_is_valid(
					candidate.record.result_id)) {
			continue;
		}
		const int32_t result = zend_tpde_value_index(
			plan, candidate.record.result_id);
		if (result >= 0) {
			const zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(result)];
			const bool lazy_scalar_join =
				(candidate.record.opcode == ZEND_MIR_OPCODE_PHI
					|| candidate.record.opcode == ZEND_MIR_OPCODE_COPY)
				&& candidate.record.representation
					== ZEND_MIR_REPRESENTATION_ZVAL;
			if (value.canonical_storage_id == storage_id
					&& (lazy_scalar_join
						|| (zend_mir_scalar_type_is_exact(
								value.exact_type)
							&& value.exact_type
								!= ZEND_MIR_SCALAR_TYPE_NULL))
					&& !value.constant
					&& !value.canonical_alias_observable) {
				const reaching_scalar_definition found{result, -1};
				if (whole_block) {
					memo[block_index] = found;
					memoized[block_index] = reaching_epoch;
				}
				return found;
			}
		}
	}

	visiting[block_index] = 1;
	const uint32_t predecessor_begin =
		plan->block_predecessor_offsets[block_index];
	const uint32_t predecessor_end =
		plan->block_predecessor_offsets[block_index + 1];
	reaching_scalar_definition merged = no_reaching_scalar;
	bool saw_cycle = false;
	for (uint32_t predecessor = predecessor_begin;
			predecessor < predecessor_end; ++predecessor) {
		const reaching_scalar_definition incoming =
			resolve_reaching_scalar(
				plan->block_predecessors[predecessor],
				plan->instruction_count, storage_id);
		if (incoming == cyclic_reaching_scalar) {
			saw_cycle = true;
			continue;
		}
		if (incoming.value_index < 0
				|| (merged.value_index >= 0
					&& !(incoming == merged))) {
			merged = no_reaching_scalar;
			break;
		}
		merged = incoming;
	}
	visiting[block_index] = 0;
	if (merged.value_index < 0 && saw_cycle) {
		return cyclic_reaching_scalar;
	}
	if (whole_block) {
		memo[block_index] = merged;
		memoized[block_index] = reaching_epoch;
	}
	return merged;
}

reaching_scalar_definition statepoint_materializer::reaching_scalar(
	uint32_t boundary_index, zend_mir_storage_id storage_id) {
	if (boundary_index >= plan->instruction_count) {
		return no_reaching_scalar;
	}
	const int32_t boundary_block = zend_tpde_block_index(
		plan, plan->instructions[boundary_index].record.block_id);
	if (boundary_block < 0) {
		return no_reaching_scalar;
	}
	++reaching_epoch;
	const reaching_scalar_definition resolved =
		resolve_reaching_scalar(static_cast<uint32_t>(boundary_block),
			boundary_index, storage_id);
	return resolved == cyclic_reaching_scalar
		? no_reaching_scalar : resolved;
}

bool statepoint_materializer::read_frame_states() {
	for (uint32_t index = 0; index < frame_count; ++index) {
		if (!view->frame_state_at(view->context, index, &frames[index])) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"materialization frame-state table is unreadable");
			return false;
		}
	}
	frame_index_capacity = frame_count == 0
		? 0 : id_index_capacity(frame_count);
	frame_index.assign(
		frame_index_capacity, {ZEND_MIR_ID_INVALID, 0});
	for (uint32_t index = 0; index < frame_count; ++index) {
		if (zend_mir_id_is_valid(frames[index].id)
				&& id_index_find(frame_index.data(), frame_index_capacity,
					frames[index].id) < 0) {
			id_index_insert(frame_index.data(), frame_index_capacity,
				frames[index].id, index);
		}
	}
	return true;
}

int32_t statepoint_materializer::definition_value(
	const zend_tpde_instruction &instruction) const {
	return zend_mir_id_is_valid(instruction.record.result_id)
		? zend_tpde_value_index(plan, instruction.record.result_id) : -1;
}

int32_t statepoint_materializer::plain_store_value(
	const zend_tpde_instruction &instruction) const {
	return instruction.record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
			&& instruction.zval_store_plain
			&& instruction.operand_count >= 1
		? zend_tpde_value_index(plan,
			zend_tpde_operand_at(plan, &instruction, 0))
		: -1;
}

void statepoint_materializer::index_definitions_and_plain_stores() {
	value_definition_offsets.assign(plan->value_count + 1, 0);
	plain_store_offsets.assign(plan->value_count + 1, 0);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const int32_t defined = definition_value(plan->instructions[index]);
		const int32_t stored = plain_store_value(plan->instructions[index]);
		if (defined >= 0) {
			++value_definition_offsets[static_cast<uint32_t>(defined) + 1];
		}
		if (stored >= 0) {
			++plain_store_offsets[static_cast<uint32_t>(stored) + 1];
		}
	}
	for (uint32_t value = 0; value < plan->value_count; ++value) {
		value_definition_offsets[value + 1] += value_definition_offsets[value];
		plain_store_offsets[value + 1] += plain_store_offsets[value];
	}
	value_definitions.resize(
		value_definition_offsets[plan->value_count]);
	plain_stores.resize(plain_store_offsets[plan->value_count]);
	{
		std::vector<uint32_t> definition_fill(value_definition_offsets.begin(),
			value_definition_offsets.end() - 1);
		std::vector<uint32_t> store_fill(plain_store_offsets.begin(),
			plain_store_offsets.end() - 1);
		for (uint32_t index = 0; index < plan->instruction_count; ++index) {
			const int32_t defined = definition_value(plan->instructions[index]);
			const int32_t stored = plain_store_value(plan->instructions[index]);
			if (defined >= 0) {
				value_definitions[
					definition_fill[static_cast<uint32_t>(defined)]++] = index;
			}
			if (stored >= 0) {
				plain_stores[store_fill[static_cast<uint32_t>(stored)]++] = index;
			}
		}
	}
}

void statepoint_materializer::size_boundary_storage_tables() {
	zend_mir_storage_id storage_bound = 0;
	for (uint32_t value = 0; value < plan->value_count; ++value) {
		const zend_mir_storage_id storage_id =
			plan->values[value].canonical_storage_id;
		if (zend_mir_id_is_valid(storage_id) && storage_id >= storage_bound
				&& storage_id < (UINT32_C(1) << 20)) {
			storage_bound = storage_id + 1;
		}
	}
	for (const zend_mir_storage_id storage_id : lazy_scalar_storages) {
		if (storage_id >= storage_bound && storage_id < (UINT32_C(1) << 20)) {
			storage_bound = storage_id + 1;
		}
	}
	boundary_storage_stamps.assign(storage_bound, 0);
	boundary_storage_materializations.assign(storage_bound, 0);
}

bool statepoint_materializer::instruction_reads_storage(
	const zend_tpde_instruction &instruction,
	const zend_mir_instruction_record &record,
	zend_mir_storage_id storage_id) {
	if (read_storages_stamp != boundary_stamp) {
		read_storages_stamp = boundary_stamp;
		reads_all_storages = false;
		boundary_read_storages.clear();
		if (instruction.has_value_operation) {
			boundary_read_storages.push_back(
				instruction.value_operation.op1_storage_id);
			boundary_read_storages.push_back(
				instruction.value_operation.op2_storage_id);
			boundary_read_storages.push_back(
				instruction.value_operation.auxiliary_storage_id);
		}
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			const int32_t operand_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, operand));
			if (operand_index >= 0) {
				boundary_read_storages.push_back(
					plan->values[operand_index].canonical_storage_id);
			}
		}
		/* A direct call reads its source-backed arguments from the
		 * frame. */
		if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| record.opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
				&& instruction.call_argument_offset
					<= plan->call_argument_count
				&& instruction.call_argument_count
					<= plan->call_argument_count
						- instruction.call_argument_offset) {
			for (uint32_t argument_index = 0;
					!reads_all_storages
						&& argument_index < instruction.call_argument_count;
					++argument_index) {
				zend_mir_call_argument_ref argument{};
				if (!zend_tpde_call_argument_at(plan,
						instruction.call_argument_offset + argument_index,
						&argument)) {
					reads_all_storages = true;
				} else {
					boundary_read_storages.push_back(
						source_descriptor_storage(source_op_array,
							argument.source_operand));
				}
			}
		}
		/* A call reads its arguments through its frame state. */
		if (!reads_all_storages
				&& zend_mir_id_is_valid(record.frame_state_id)) {
			const int32_t frame_position = id_index_find(
				frame_index.data(), frame_index_capacity,
				record.frame_state_id);
			if (frame_position < 0) {
				reads_all_storages = true;
			} else {
				const zend_mir_frame_state_ref &frame =
					frames[static_cast<uint32_t>(frame_position)];
				for (uint32_t slot_index = 0;
						!reads_all_storages && slot_index < frame.slots.count;
						++slot_index) {
					zend_mir_frame_slot_ref slot;
					if (!view->frame_slot_at(view->context,
							frame.slots.offset + slot_index, &slot)) {
						reads_all_storages = true;
						break;
					}
					const int32_t slot_value =
						zend_mir_id_is_valid(slot.value_id)
							? zend_tpde_value_index(plan, slot.value_id)
							: -1;
					if (slot_value >= 0) {
						boundary_read_storages.push_back(
							plan->values[slot_value].canonical_storage_id);
					}
				}
			}
		}
		std::ranges::sort(boundary_read_storages);
	}
	return reads_all_storages
		|| std::ranges::binary_search(boundary_read_storages, storage_id);
}

void statepoint_materializer::add_read_storage(
	std::vector<zend_mir_storage_id> &read_storages,
	zend_mir_storage_id storage_id) const {
	if (zend_mir_id_is_valid(storage_id)
			&& std::binary_search(lazy_scalar_storages.begin(),
				lazy_scalar_storages.end(), storage_id)
			&& std::ranges::find(read_storages, storage_id)
				== read_storages.end()) {
		read_storages.push_back(storage_id);
	}
}

void statepoint_materializer::collect_lazy_reads(
	const zend_tpde_instruction &instruction,
	const zend_mir_instruction_record &record,
	std::vector<zend_mir_storage_id> &read_storages) const {
	if (instruction.has_value_operation) {
		const zend_mir_executable_value_ref &operation =
			instruction.value_operation;
		if (!(instruction.mutation_lazy_scalar
				&& instruction.mutation_storage_id
					== operation.op1_storage_id)) {
			add_read_storage(read_storages, operation.op1_storage_id);
		}
		add_read_storage(read_storages, operation.op2_storage_id);
		add_read_storage(read_storages, operation.auxiliary_storage_id);
	}
	const bool register_operation =
		record.opcode == ZEND_MIR_OPCODE_PHI
		|| record.opcode == ZEND_MIR_OPCODE_COPY
		|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
		|| record.opcode == ZEND_MIR_OPCODE_CONSTANT
		|| record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
		|| (record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
			&& record.opcode <= ZEND_MIR_OPCODE_SCALAR_DROP);
	if (!register_operation) {
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			const int32_t operand_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, operand));
			if (operand_index >= 0) {
				add_read_storage(read_storages, plan->values[
					operand_index].canonical_storage_id);
			}
		}
	}
}

/* This boundary's materialization of the storage, or UINT32_MAX. */
uint32_t statepoint_materializer::boundary_materialization(
	zend_mir_storage_id storage_id) const {
	const zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	if (storage_id < boundary_storage_stamps.size()) {
		return boundary_storage_stamps[storage_id] == boundary_stamp
			? boundary_storage_materializations[storage_id]
			: UINT32_MAX;
	}
	for (uint32_t materialization_index =
			instruction.materialization_offset;
			materialization_index < materializations.size();
			++materialization_index) {
		if (materializations[materialization_index].storage_id
				== storage_id) {
			return materialization_index;
		}
	}
	return UINT32_MAX;
}

bool statepoint_materializer::duplicate_storage(
	zend_mir_storage_id storage_id) const {
	return boundary_materialization(storage_id) != UINT32_MAX;
}

/*
 * A value materialized from its defining instruction is a boxed zval
 * unless it is a scalar machine value, such as a typed-tier result
 * (ADR 0024), which has a single payload part.
 */
zend_tpde_machine_value_kind statepoint_materializer::defined_machine_kind(
	const zend_tpde_source_value_binding &binding) const {
	if (binding.value_index >= 0
			&& static_cast<uint32_t>(binding.value_index)
				< plan->value_count) {
		const zend_tpde_machine_value_kind kind =
			plan->values[binding.value_index].machine_kind;
		if (zend_tpde_machine_kind_is_scalar(kind)) {
			return kind;
		}
	}
	return ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
}

/*
 * A typed-tier value (ADR 0024) that a helper or call reads from the
 * frame is stored into its slot right after its definition by a plain
 * ZVAL_STORE; publishing it again at the boundary is redundant.
 */
bool statepoint_materializer::stored_after_definition(
	zend_mir_storage_id storage_id,
	int32_t source_value_index,
	int32_t source_definition_instruction_index) const {
	const uint32_t index = current_boundary;
	if (!zend_tpde_linux_inline_forms || source_value_index < 0) {
		return false;
	}
	const uint32_t value = static_cast<uint32_t>(source_value_index);
	if (source_definition_instruction_index < 0) {
		/* The nearest definition before the boundary. */
		const auto begin = value_definitions.begin()
			+ value_definition_offsets[value];
		const auto end = value_definitions.begin()
			+ value_definition_offsets[value + 1];
		const auto after = std::lower_bound(begin, end, index);
		if (after == begin) {
			return false;
		}
		source_definition_instruction_index =
			static_cast<int32_t>(*(after - 1));
	}
	const auto end = plain_stores.begin()
		+ plain_store_offsets[value + 1];
	for (auto store = std::upper_bound(
				plain_stores.begin() + plain_store_offsets[value], end,
				static_cast<uint32_t>(
					source_definition_instruction_index));
			store != end && *store < index; ++store) {
		if (plan->instructions[*store].zval_store_storage_id
				== storage_id) {
			return true;
		}
	}
	return false;
}

void statepoint_materializer::append_materialization(
	uint32_t value_index,
	zend_mir_storage_id storage_id,
	zend_tpde_machine_value_kind machine_kind,
	int32_t source_value_index,
	int32_t source_definition_instruction_index) {
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	if (!zend_mir_id_is_valid(storage_id)
			|| duplicate_storage(storage_id)
			|| stored_after_definition(storage_id, source_value_index,
				source_definition_instruction_index)) {
		return;
	}
	if (storage_id < boundary_storage_stamps.size()) {
		boundary_storage_stamps[storage_id] = boundary_stamp;
		boundary_storage_materializations[storage_id] =
			static_cast<uint32_t>(materializations.size());
	}
	materializations.push_back({
		value_index,
		storage_id,
		machine_kind,
		source_value_index,
		source_definition_instruction_index,
	});
	++instruction.materialization_count;
}

void statepoint_materializer::append_source_materialization(
	zend_mir_storage_id storage_id,
	zend_tpde_machine_value_kind machine_kind,
	int32_t source_value_index,
	int32_t source_definition_instruction_index) {
	if (const uint32_t existing =
			boundary_materialization(storage_id);
			existing != UINT32_MAX) {
		materializations[existing] = {
			UINT32_MAX, storage_id, machine_kind,
			source_value_index,
			source_definition_instruction_index,
		};
		return;
	}
	append_materialization(
		UINT32_MAX, storage_id, machine_kind,
		source_value_index, source_definition_instruction_index);
}

int32_t statepoint_materializer::source_scalar_definition(
	zend_mir_storage_id storage_id) const {
	const uint32_t index = current_boundary;
	for (uint32_t cursor = index; cursor > 0; --cursor) {
		const uint32_t candidate_index = cursor - 1;
		const zend_tpde_instruction &candidate =
			plan->instructions[candidate_index];
		const zend_mir_instruction_record candidate_record =
			zend_tpde_instruction_record_at(plan, &candidate);
		if (candidate_record.opcode == ZEND_MIR_OPCODE_CONSTANT
				|| candidate_record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
			continue;
		}
		if (candidate_record.opcode
				== ZEND_MIR_OPCODE_VALUE_UNARY_OP
				&& candidate.has_value_operation
				&& candidate.value_operation.result_storage_id
					== storage_id
				&& candidate.value_operation.source_opcode
					== ZEND_STRLEN) {
			return static_cast<int32_t>(candidate_index);
		}
		/*
		 * A source result is a materialization input only for its
		 * direct consumer: temporary storage ids are reused, so looking
		 * past another instruction could bind an unrelated producer.
		 */
		return int32_t{-1};
	}
	return int32_t{-1};
}

bool statepoint_materializer::materialize_frame_state(
	const zend_mir_instruction_record &record) {
	const uint32_t index = current_boundary;
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	if (zend_mir_id_is_valid(record.frame_state_id)) {
		const int32_t frame_index_value = id_index_find(
			frame_index.data(), frame_index_capacity,
			record.frame_state_id);
		const zend_mir_frame_state_ref *frame = frame_index_value < 0
			? nullptr
			: &frames[static_cast<uint32_t>(frame_index_value)];
		if (frame == nullptr || frame->function_id != plan->function.id
				|| frame->slots.offset
					> view->frame_slot_count(view->context)
				|| frame->slots.count
					> view->frame_slot_count(view->context)
						- frame->slots.offset) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"observable instruction lacks its materialization frame");
			return false;
		}

		const bool function_entry =
			record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			&& frame->safepoint_class
				== ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
		const bool generator_suspend = instruction.has_value_operation
			&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
				|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
				|| record.opcode
					== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM);
		if (!function_entry) {
			for (uint32_t slot_index = 0;
					slot_index < frame->slots.count; ++slot_index) {
				zend_mir_frame_slot_ref slot{};
				if (!view->frame_slot_at(view->context,
						frame->slots.offset + slot_index, &slot)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"materialization frame slot is unreadable");
					return false;
				}
				if (slot.materialization
							!= ZEND_MIR_MATERIALIZATION_MATERIALIZED
						|| !zend_mir_id_is_valid(slot.value_id)) {
					continue;
				}
				int32_t value_index =
					zend_tpde_value_index(plan, slot.value_id);
				if (value_index < 0) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"materialization references an unknown value");
					return false;
				}
				const zend_mir_storage_id storage_id =
					plan->values[static_cast<uint32_t>(value_index)]
						.canonical_storage_id;
				/* A suspension's frame is the state at its resume. */
				if (generator_suspend) {
					value_index = static_cast<int32_t>(
						generator_suspend_value_index(plan,
							generator_definitions.data(), index,
							static_cast<uint32_t>(value_index)));
				}
				const zend_tpde_value &value =
					plan->values[static_cast<uint32_t>(value_index)];
				if (value.slot_state
							!= ZEND_TPDE_CANONICAL_SLOT_DIRTY
						|| !zend_mir_id_is_valid(storage_id)
						|| value.constant) {
					continue;
				}
				append_materialization(
					static_cast<uint32_t>(value_index),
					storage_id,
					value.machine_kind, value_index, -1);
			}
		}
	}
	return true;
}

void statepoint_materializer::append_reaching_scalar(
	zend_mir_storage_id storage_id) {
	const uint32_t index = current_boundary;
	const reaching_scalar_definition reaching =
		reaching_scalar(index, storage_id);
	if (reaching.value_index < 0) {
		return;
	}
	const zend_tpde_value &value =
		plan->values[
			static_cast<uint32_t>(reaching.value_index)];
	append_materialization(
		reaching.instruction_index < 0
			? static_cast<uint32_t>(reaching.value_index)
			: UINT32_MAX,
		storage_id, value.machine_kind,
		reaching.value_index, reaching.instruction_index);
}

void statepoint_materializer::materialize_lazy_scalars(
	const zend_mir_instruction_record &record) {
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	/*
	 * A helper or call observes an operand-observed CV only through
	 * its operands; statepoints, returns and probes observe the frame.
	 */
	const bool operand_boundary = operand_observed_plan
		&& !instruction.debug_probe
		&& record.opcode != ZEND_MIR_OPCODE_STATEPOINT
		&& record.opcode != ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL;
	for (zend_mir_storage_id storage_id : lazy_scalar_storages) {
		if (operand_boundary
				&& storage_id < plan->operand_observed_storage_count
				&& plan->operand_observed_storages[storage_id] != 0
				&& !instruction_reads_storage(instruction, record,
					storage_id)) {
			continue;
		}
		append_reaching_scalar(storage_id);
	}
}

void statepoint_materializer::materialize_generator_suspend(
	const zend_mir_instruction_record &record) {
	const uint32_t index = current_boundary;
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	/*
	 * A native generator resumes register-authoritative values from their
	 * canonical slots, so the suspension publishes its suspend-live
	 * values. Reaching definitions do not suffice: a loop value copied
	 * from a PHI only for use after the yield, such as an induction
	 * variable, has no ZVAL_STORE before it.
	 */
	if (instruction.has_value_operation
			&& record.source_position_id != UINT32_MAX
			&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
				|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
				|| record.opcode
					== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
		const uint32_t resume = generator_resume_at_target(
			plan, record.source_position_id + 1);
		zend_tpde_visit_generator_resume_values(plan, resume,
				[&](uint32_t live_index) {
				const uint32_t value_index =
					generator_suspend_value_index(plan,
						generator_definitions.data(), index, live_index);
				const zend_tpde_value &value =
					plan->values[value_index];
				/*
				 * A pointer whose slot is authoritative may have been
				 * replaced there by a helper that separated it (an
				 * append to a shared array); the resume reloads it from
				 * the slot, so storing the register copy would restore
				 * the old pointer.
				 */
				if (!value.register_authoritative
						&& zend_tpde_machine_kind_is_value_pointer(
							value.machine_kind)) {
					return true;
				}
				append_materialization(
					value_index,
					plan->values[live_index].canonical_storage_id,
					value.machine_kind, value_index, -1);
				return true;
			});
	}
}

bool statepoint_materializer::materialize_helper_inputs(
	const zend_mir_instruction_record &record) {
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	/*
	 * Frame states of canonical-location modules omit duplicated slots,
	 * but runtime helpers read their explicit operands from the Zend
	 * frame: publish register-authoritative inputs, including private
	 * typed-call results without a MIR result. RETURN_BY_REF always
	 * returns through its helper, which reads the operand's slot.
	 */
	if ((instruction.runtime_helper != ZEND_NATIVE_HELPER_COUNT
				|| (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					&& instruction.value_operation.source_opcode
						== ZEND_RETURN_BY_REF))
			&& instruction.has_value_operation) {
		const struct {
			zend_tpde_source_value_binding binding;
			zend_mir_storage_id storage_id;
		} inputs[] = {
			{instruction.source_op1_binding,
				instruction.value_operation.op1_storage_id},
			{instruction.source_op2_binding,
				instruction.value_operation.op2_storage_id},
			{instruction.source_auxiliary_binding,
				instruction.value_operation.auxiliary_storage_id},
		};
		for (const auto &input : inputs) {
			if (!zend_mir_id_is_valid(input.storage_id)) {
				continue;
			}
			const int32_t source_definition =
				source_scalar_definition(input.storage_id);
			if (source_definition >= 0) {
				append_source_materialization(
					input.storage_id,
					ZEND_TPDE_MACHINE_VALUE_I64,
					input.binding.value_index, source_definition);
				continue;
			}
			if (input.binding.definition_instruction_index >= 0) {
				append_materialization(
					UINT32_MAX, input.storage_id,
					defined_machine_kind(input.binding),
					input.binding.value_index,
					input.binding.definition_instruction_index);
				continue;
			}
			if (input.binding.value_index >= 0) {
				const uint32_t value_index =
					static_cast<uint32_t>(input.binding.value_index);
				if (value_index >= plan->value_count) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source materialization references an unknown value");
					return false;
				}
				const zend_tpde_value &value =
					plan->values[value_index];
				if (!value.constant && value.register_authoritative
						&& value.slot_state
							== ZEND_TPDE_CANONICAL_SLOT_DIRTY) {
					append_materialization(
						value_index, input.storage_id,
						value.machine_kind,
						input.binding.value_index,
						input.binding.definition_instruction_index);
				}
			}
		}
	}
	return true;
}

bool statepoint_materializer::materialize_call_arguments(
	const zend_mir_instruction_record &record) {
	zend_tpde_instruction &instruction =
		plan->instructions[current_boundary];
	/*
	 * Direct call helpers read their source-backed arguments from the
	 * Zend frame, while a producer may keep a newer pointer or boxed
	 * value in registers (FETCH_DIM_W followed by SEND_REF): publish
	 * those dirty argument slots.
	 */
	if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
			&& instruction.call_argument_offset
				<= plan->call_argument_count
			&& instruction.call_argument_count
				<= plan->call_argument_count
					- instruction.call_argument_offset) {
		if (instruction.call_argument_count != 0
				&& plan->call_argument_bindings == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"call materialization binding table is missing");
			return false;
		}
		for (uint32_t argument_index = 0;
				argument_index < instruction.call_argument_count;
				++argument_index) {
			const uint32_t frozen_index =
				instruction.call_argument_offset + argument_index;
			zend_mir_call_argument_ref argument{};
			if (!zend_tpde_call_argument_at(
					plan, frozen_index, &argument)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"call materialization argument is unreadable");
				return false;
			}
			const zend_mir_storage_id storage_id =
				source_descriptor_storage(
					source_op_array, argument.source_operand);
			if (!zend_mir_id_is_valid(storage_id)) {
				continue;
			}
			const zend_tpde_source_value_binding &binding =
				plan->call_argument_bindings[frozen_index];
			const int32_t source_definition =
				source_scalar_definition(storage_id);
			if (source_definition >= 0) {
				append_source_materialization(
					storage_id,
					ZEND_TPDE_MACHINE_VALUE_I64,
					binding.value_index, source_definition);
				continue;
			}
			if (binding.definition_instruction_index >= 0) {
				append_materialization(
					UINT32_MAX, storage_id,
					defined_machine_kind(binding),
					binding.value_index,
					binding.definition_instruction_index);
				continue;
			}
			if (binding.value_index < 0
					|| static_cast<uint32_t>(binding.value_index)
						>= plan->value_count) {
				continue;
			}
			const uint32_t value_index =
				static_cast<uint32_t>(binding.value_index);
			const zend_tpde_value &value =
				plan->values[value_index];
			if (!value.constant && value.register_authoritative
					&& value.slot_state
						== ZEND_TPDE_CANONICAL_SLOT_DIRTY) {
				append_materialization(
					value_index, storage_id, value.machine_kind,
					binding.value_index,
					binding.definition_instruction_index);
			}
		}
	}
	return true;
}

/* Plans the materializations of one instruction when it is an observable
 * boundary or reads a lazy CV from its frame slot. */
bool statepoint_materializer::materialize_boundary(uint32_t index) {
	zend_tpde_instruction &instruction = plan->instructions[index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &instruction);
	const bool observable_boundary =
		(record.opcode == ZEND_MIR_OPCODE_STATEPOINT
		|| record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
		|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
		|| instruction.runtime_helper != ZEND_NATIVE_HELPER_COUNT
		|| instruction.debug_probe)
		&& !(record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
			&& instruction.zval_store_lazy_scalar);

	/*
	 * An inline form that reads a lazy CV from its frame slot observes
	 * it like a helper: store the reaching value first.  Scalar machine
	 * operations, joins and stores take the register value.
	 */
	std::vector<zend_mir_storage_id> read_storages;
	if (!observable_boundary && !lazy_scalar_storages.empty()) {
		collect_lazy_reads(instruction, record, read_storages);
	}

	instruction.materialization_offset =
		static_cast<uint32_t>(materializations.size());
	instruction.materialization_count = 0;
	if (!observable_boundary && read_storages.empty()) {
		return true;
	}

	++boundary_stamp;
	current_boundary = index;
	if (!observable_boundary) {
		for (zend_mir_storage_id storage_id : read_storages) {
			const reaching_scalar_definition reaching =
				reaching_scalar(index, storage_id);
			if (reaching.value_index >= 0) {
				append_materialization(
					reaching.instruction_index < 0
						? static_cast<uint32_t>(reaching.value_index)
						: UINT32_MAX,
					storage_id,
					plan->values[static_cast<uint32_t>(
						reaching.value_index)].machine_kind,
					reaching.value_index, reaching.instruction_index);
			}
		}
		return true;
	}
	if (!materialize_frame_state(record)) {
		return false;
	}
	materialize_lazy_scalars(record);
	materialize_generator_suspend(record);
	/*
	 * A local dynamic read may select any CV from its runtime name. In
	 * canonical-location modules frame states omit duplicated slots, so the
	 * cold helper boundary must publish every register-authoritative CV that
	 * has a sparse reaching definition, not only the explicit name operand.
	 */
	if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			&& instruction.has_value_operation
			&& instruction.value_operation.extended_value
				== ZEND_FETCH_LOCAL) {
		for (zend_mir_storage_id storage_id = 0;
				storage_id < source_op_array->last_var; ++storage_id) {
			append_reaching_scalar(storage_id);
		}
	}
	return materialize_helper_inputs(record)
		&& materialize_call_arguments(record);
}

bool statepoint_materializer::publish() {
	if (materializations.size() > MAX_RECORDS) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"statepoint materialization plan exceeds executable bound");
		return false;
	}
	plan->materialization_count =
		static_cast<uint32_t>(materializations.size());
	if (materializations.empty()) {
		return true;
	}
	plan->materializations.reset(static_cast<zend_tpde_materialization *>(
		std::malloc(materializations.size()
			* sizeof(*plan->materializations))));
	if (plan->materializations == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate statepoint materialization plan");
		return false;
	}
	std::memcpy(plan->materializations, materializations.data(),
		materializations.size() * sizeof(*plan->materializations));
	return true;
}

} // namespace

bool freeze_statepoint_materializations(
	zend_tpde_plan *plan,
	const zend_mir_view *view,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag)
{
	statepoint_materializer materializer{plan, view, source_op_array, diag};
	return materializer.freeze();
}

/*
 * The deoptimization exits of the specialized members (ADR 0025 section 4).
 * A guarded operation exits where its generic copy has a landing (both
 * members lower the same MIR; the copy's adaptor decides which resume IDs
 * land, and the component adaptor adopts it). The machine CFG keeps an
 * exit's cold-to-continuation edge, so the specialized member's frozen CFG
 * stays as it is.
 */
bool freeze_deopt_exits(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		zend_native_diagnostic *diag) {
	for (uint32_t index = 0; index < component_count; ++index) {
		zend_tpde_plan &specialized = plans[index];
		if (specialized.deopt_generic_member_plus_one == 0
				|| specialized.deopt_generic_member_plus_one
					> component_count) {
			continue;
		}
		const zend_tpde_plan &generic =
			plans[specialized.deopt_generic_member_plus_one - 1];
		if (generic.deopt_resume_count == 0
				|| generic.instruction_count != specialized.instruction_count
				|| generic.value_count != specialized.value_count) {
			continue;
		}
		if (generic.deopt_landing_adaptor == nullptr) {
			generic.deopt_landing_adaptor =
				new zend::native::tpde::ZendIRAdaptor{&generic,
					std::span<const zend_tpde_plan *const>{
						component_plans, component_count}};
		}
		const zend::native::tpde::ZendIRAdaptor &landings =
			*generic.deopt_landing_adaptor;
		if (!landings.valid()) {
			continue;
		}
		bool exits = false;
		for (uint32_t resume = 0; resume < generic.deopt_resume_count;
				++resume) {
			const uint32_t instruction =
				generic.deopt_resume_instructions[resume];
			if (!landings.deopt_resume_landed(resume)
					|| instruction >= specialized.instruction_count) {
				continue;
			}
			zend_tpde_instruction &exit = specialized.instructions[instruction];
			if (exit.record.opcode
						!= generic.instructions[instruction].record.opcode
					|| exit.record.source_position_id
						!= generic.deopt_resume_targets[resume]
					|| (exit.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) == 0) {
				continue;
			}
			exit.deopt_exit_resume_plus_one = resume + 1;
			exits = true;
		}
		if (!exits) {
			continue;
		}
		require_runtime_helper(&specialized,
			ZEND_NATIVE_HELPER_DEOPT_TRANSFER);
	}
	return true;
}

void destroy_deopt_landing_adaptor(zend_tpde_plan *plan) {
	delete plan->deopt_landing_adaptor;
}

} // namespace zend::native::tpde::plan_detail
