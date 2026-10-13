// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_layout.hpp"

/*
 * The layout profile and training of the backends that lay out code by
 * profile (Linux x86-64); the others define these entry points to do
 * nothing.
 */

namespace zend::native::tpde {

LayoutProfile layout_profile;

/* At most this many calls per request make a function rare
 * (zend_tpde_layout_rare_function()); ZEND_NATIVE_RARE_CALLS=0 disables
 * it. Read when the binary loads, before FPM clears the environment. */
const uint64_t layout_rare_calls = [] {
	const char *value = std::getenv("ZEND_NATIVE_RARE_CALLS");
	return value != nullptr && value[0] != '\0'
		? std::strtoull(value, nullptr, 10) : uint64_t{12};
}();

LayoutTraining layout_training;

} // namespace zend::native::tpde

using zend::native::tpde::LayoutTraining;
using zend::native::tpde::layout_function_key;
using zend::native::tpde::layout_profile;
using zend::native::tpde::layout_rare_calls;
using zend::native::tpde::layout_training;

extern "C" bool zend_tpde_layout_training(void)
{
	return layout_training.active();
}

/*
 * What of a member's code to keep small because the profile saw its
 * generic body called rarely: at most layout_rare_calls times per request
 * over the trained requests. A native call entry of its own then costs
 * more in the bytes it brings into the caches than it saves on so few
 * calls.
 */
uint32_t zend_tpde_layout_rare_function(
	uint32_t index, const zend_tpde_plan *plan)
{
	if (layout_profile.all) {
		/* "*" treats every other member as rare, to test it. */
		return (index & 1) != 0 ? ZEND_TPDE_RARE_NO_CALL_ENTRY : 0;
	}
	if (layout_profile.requests < 16 || layout_rare_calls == 0) {
		return 0;
	}
	const auto counted = layout_profile.blocks.find(
		layout_function_key(index, false, plan));
	if (counted == layout_profile.blocks.end()) {
		return 0;
	}
	const auto calls = counted->second.find(static_cast<uint32_t>(
		zend_tpde_block_index(plan, plan->function.entry_block_id)));
	return calls != counted->second.end() && calls->second != 0
			&& calls->second <= layout_rare_calls * layout_profile.requests
		? ZEND_TPDE_RARE_NO_CALL_ENTRY : 0;
}

extern "C" uint64_t zend_tpde_layout_training_session(void)
{
	return layout_training.switched ? 0 : layout_training.session;
}

/* FPM workers exit without destructors: every sixteenth request writes.
 * The request that completes a switching run's training writes the
 * profile and marks it ready for every process. */
extern "C" void zend_tpde_layout_training_flush(void)
{
	LayoutTraining &training = layout_training;
	if (!training.active()) {
		return;
	}
	const uint64_t total = __atomic_add_fetch(
		&training.counters[0].ordinal, 1, __ATOMIC_RELAXED);
	uint64_t phase = LayoutTraining::phase_training;
	if (training.switch_requests != 0 && total >= training.switch_requests
			&& __atomic_compare_exchange_n(&training.counters[0].taken,
				&phase, LayoutTraining::phase_writing, false,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		training.write();
		__atomic_store_n(&training.counters[0].taken,
			LayoutTraining::phase_ready, __ATOMIC_RELEASE);
		training.restart_wanted = true;
		return;
	}
	if (++training.requests % 16 == 0) {
		training.write();
	}
}

/* At request start: a process whose run's profile is ready stops
 * training and compiles with the profile. */
extern "C" bool zend_tpde_layout_switch(void)
{
	LayoutTraining &training = layout_training;
	if (!training.active() || training.switch_requests == 0
			|| __atomic_load_n(&training.counters[0].taken,
				__ATOMIC_ACQUIRE) != LayoutTraining::phase_ready) {
		return false;
	}
	training.switched = true;
	if (training.path != nullptr) {
		layout_profile.load(training.path);
	} else {
		training.select(layout_profile);
	}
	return true;
}

/*
 * A server (FPM) trains by default: the counters are mapped in its master,
 * before the workers fork, and the profile stays in that memory. Not when
 * the environment chose a profile, a training or ZEND_NATIVE_LAYOUT=off.
 */
extern "C" void zend_tpde_layout_automatic(void)
{
	const char *mode = std::getenv("ZEND_NATIVE_LAYOUT");
	const char *profile = std::getenv("ZEND_NATIVE_LAYOUT_PROFILE");
	if (layout_training.counters != nullptr
			|| (mode != nullptr && mode[0] != '\0')
			|| (profile != nullptr && profile[0] != '\0')) {
		return;
	}
	layout_training.switch_requests = LayoutTraining::requested_switch();
	if (layout_training.switch_requests != 0) {
		layout_training.map(nullptr);
	}
}

/* Whether this process wrote the profile and OPcache should recompile the
 * cached scripts with it; asks once. */
extern "C" bool zend_tpde_layout_take_restart(void)
{
	const bool wanted = layout_training.restart_wanted;
	layout_training.restart_wanted = false;
	return wanted;
}
