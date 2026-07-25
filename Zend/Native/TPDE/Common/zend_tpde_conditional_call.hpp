// SPDX-License-Identifier: PHP-3.01
#pragma once

#include <tpde/CompilerBase.hpp>

namespace zend::native::tpde {

template <typename Compiler>
struct ConditionalCallRegisterState {
	using AsmReg = typename Compiler::AsmReg;

	struct Entry {
		AsmReg reg;
		::tpde::ValLocalIdx local_idx;
		uint32_t part;
		bool modified;
	};

	::tpde::util::SmallVector<Entry, 16> entries;
};

/*
 * A CallBuilder mutates the compiler's register assignment while emitting
 * spills. If the call is reached only from an inline slow path, the
 * fallthrough path has not executed those spills. Capture and restore the
 * assignment state around the call so both machine paths meet with identical
 * physical and logical register state without penalizing the fast path.
 */
template <typename Compiler>
ConditionalCallRegisterState<Compiler>
capture_conditional_call_register_state(Compiler &compiler)
{
	ConditionalCallRegisterState<Compiler> state;

	assert(compiler.may_change_value_state());
	for (auto reg_id : compiler.register_file.used_regs()) {
		::tpde::Reg reg{reg_id};
		const ::tpde::ValLocalIdx local_idx =
			compiler.register_file.reg_local_idx(reg);
		if (compiler.register_file.is_fixed(reg)
				|| local_idx == Compiler::INVALID_VAL_LOCAL_IDX) {
			continue;
		}
		const uint32_t part = compiler.register_file.reg_part(reg);
		::tpde::AssignmentPartRef assignment_part{
			compiler.val_assignment(local_idx), part};
		assert(assignment_part.register_valid()
			&& assignment_part.get_reg() == reg);
		state.entries.push_back(typename
			ConditionalCallRegisterState<Compiler>::Entry{
				typename Compiler::AsmReg{reg_id},
				local_idx,
				part,
				assignment_part.modified()});
	}
	return state;
}

template <typename Compiler>
void restore_conditional_call_register_state(
	Compiler &compiler,
	const ConditionalCallRegisterState<Compiler> &state)
{
	assert(compiler.may_change_value_state());
	for (const auto &entry : state.entries) {
		::tpde::ValueAssignment *assignment =
			compiler.val_assignment(entry.local_idx);
		assert(assignment != nullptr);
		::tpde::AssignmentPartRef assignment_part{
			assignment, entry.part};
		if (assignment_part.register_valid()) {
			assert(assignment_part.get_reg() == entry.reg);
			continue;
		}
		assert(!compiler.register_file.is_used(entry.reg));
		compiler.reload_to_reg(entry.reg, assignment_part);
		assignment_part.set_reg(entry.reg);
		assignment_part.set_register_valid(true);
		/*
		 * The slow path spilled the current value, while the fast path did
		 * not. Preserve the pre-branch dirty state so future eviction is
		 * correct on both paths.
		 */
		assignment_part.set_modified(entry.modified);
		compiler.register_file.mark_used(
			entry.reg, entry.local_idx, entry.part);
		compiler.register_file.mark_clobbered(entry.reg);
	}
}

} // namespace zend::native::tpde
