// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

#include <glob.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace zend::native::tpde {

/*
 * Profile-guided layout (ZEND_NATIVE_LAYOUT_PROFILE=path): per function key,
 * the ordinals of conditional jumps nearly always taken over code that
 * rarely runs. A line "key ordinal" names one; lines "key ordinal taken
 * fallthrough" of path and of the training files path.* are summed and
 * select the jumps taken at least nine times in ten over at least
 * layout_minimum_executions runs. Read when the binary loads, before FPM
 * clears the environment of its workers.
 */
inline constexpr uint64_t layout_minimum_executions = 256;

/*
 * ZEND_NATIVE_LAYOUT=path combines both: an existing path is the profile,
 * otherwise this run trains into it. After ZEND_NATIVE_LAYOUT_REQUESTS
 * requests (default layout_switch_requests) of all processes sharing the
 * training memory, the profile is written and every process switches to
 * it; OPcache then recompiles the cached scripts with it.
 */
inline constexpr uint64_t layout_switch_requests = 128;
inline const char *layout_environment(const char *name, bool profile) {
	if (const char *value = std::getenv(name); value != nullptr
			&& value[0] != '\0') {
		return value;
	}
	const char *both = std::getenv("ZEND_NATIVE_LAYOUT");
	if (both == nullptr || both[0] == '\0') {
		return nullptr;
	}
	return (::access(both, R_OK) == 0) == profile ? both : nullptr;
}

/* Ordinals with this bit count the entries of a block: the rest is the
 * adaptor's block index. */
inline constexpr uint32_t layout_block_ordinal = UINT32_C(1) << 31;
/* A block entered in fewer than one of a hundred calls of its function,
 * over at least this many calls, goes to the cold area. */
inline constexpr uint64_t layout_minimum_calls = 64;

/* The count line of the number of requests trained: key 0 with this
 * ordinal. */
inline constexpr uint32_t layout_requests_ordinal = UINT32_MAX;

struct LayoutProfile {
	std::unordered_map<uint64_t, std::unordered_set<uint32_t>> jumps;
	/* Per function key, the entries of each counted block. */
	std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>>
		blocks;
	/* The requests the counts were taken over, 0 when unknown. */
	uint64_t requests = 0;
	/* "*" moves every eligible region, to test the transformation. */
	bool all = false;
	using Counts = std::unordered_map<uint64_t,
		std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>>;
	LayoutProfile() {
		load(layout_environment("ZEND_NATIVE_LAYOUT_PROFILE", true));
	}
	/* The jumps taken at least nine times in ten over enough runs. */
	void select(const Counts &counts) {
		for (const auto &[key, ordinals] : counts) {
			for (const auto &[ordinal, outcome] : ordinals) {
				if (key == 0 && ordinal == layout_requests_ordinal) {
					requests += outcome.first;
					continue;
				}
				if ((ordinal & layout_block_ordinal) != 0) {
					blocks[key][ordinal & ~layout_block_ordinal] +=
						outcome.first;
					continue;
				}
				const uint64_t executions = outcome.first + outcome.second;
				if (executions >= layout_minimum_executions
						&& outcome.first * 10 >= executions * 9) {
					jumps[key].insert(ordinal);
				}
			}
		}
	}
	void load(const char *path) {
		jumps.clear();
		blocks.clear();
		requests = 0;
		all = false;
		if (path == nullptr || path[0] == '\0') {
			return;
		}
		if (path[0] == '*' && path[1] == '\0') {
			all = true;
			return;
		}
		Counts counts;
		read(path, counts);
		const std::string pattern = std::string(path) + ".*";
		glob_t files{};
		if (::glob(pattern.c_str(), 0, nullptr, &files) == 0) {
			for (size_t i = 0; i < files.gl_pathc; ++i) {
				read(files.gl_pathv[i], counts);
			}
		}
		::globfree(&files);
		select(counts);
	}
	void read(const char *path, std::unordered_map<uint64_t,
			std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>>
				&counts) {
		FILE *file = std::fopen(path, "r");
		if (file == nullptr) {
			return;
		}
		char line[128];
		while (std::fgets(line, sizeof(line), file) != nullptr) {
			unsigned long long key, taken, fallthrough;
			unsigned int ordinal;
			const int fields = std::sscanf(line, "%llx %u %llu %llu",
				&key, &ordinal, &taken, &fallthrough);
			if (fields == 2) {
				jumps[key].insert(ordinal);
			} else if (fields == 4) {
				auto &outcome = counts[key][ordinal];
				outcome.first += taken;
				outcome.second += fallthrough;
			}
		}
		std::fclose(file);
	}
};
extern LayoutProfile layout_profile;


/* A function's key in the profile: stable from one compilation of the same
 * code to the next. */
inline uint64_t layout_function_key(uint32_t index, bool typed_body,
		const zend_tpde_plan *plan) {
	uint64_t key = UINT64_C(14695981039346656037);
	auto mix = [&](uint64_t value) {
		key = (key ^ value) * UINT64_C(1099511628211);
	};
	mix(index);
	mix(typed_body ? 1 : 0);
	if (plan != nullptr) {
		mix(plan->instruction_count);
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			const zend_mir_instruction_record &record =
				plan->instructions[i].record;
			mix(record.opcode);
			mix(record.source_position_id);
		}
	}
	return key;
}

/*
 * Layout training (ZEND_NATIVE_LAYOUT_TRAIN=path): each eligible jump gets
 * a counter in shared memory mapped when the binary loads (in FPM's
 * master, so that every worker shares it at the same address), which also
 * records the jump's function key and ordinal.
 */
struct LayoutTraining {
	struct Counter {
		uint64_t key;
		uint64_t ordinal;
		uint64_t taken;
		uint64_t fallthrough;
	};
	static constexpr size_t capacity = size_t{1} << 21;
	/* A copy: FPM overwrites the environment with its process titles. */
	std::string file;
	const char *path = nullptr;
	/* counters[0].key is the number of counters in use. */
	Counter *counters = nullptr;
	/* Tells this run's compiled code from another's: the mapping, the
	 * process that made it and when. */
	uint64_t session = 0;
	uint32_t requests = 0;
	/* The shared request count at which a ZEND_NATIVE_LAYOUT run switches
	 * to its profile, 0 for a run that only trains. counters[0].ordinal
	 * counts the requests, counters[0].taken is the phase. */
	uint64_t switch_requests = 0;
	static constexpr uint64_t phase_training = 0;
	static constexpr uint64_t phase_writing = 1;
	static constexpr uint64_t phase_ready = 2;
	/* This process compiles with the profile. */
	bool switched = false;
	/* This process wrote the profile: OPcache recompiles with it. */
	bool restart_wanted = false;
	LayoutTraining() {
		const char *train = std::getenv("ZEND_NATIVE_LAYOUT_TRAIN");
		const char *name =
			layout_environment("ZEND_NATIVE_LAYOUT_TRAIN", false);
		if (name == nullptr || name[0] == '\0'
				|| std::strcmp(name, "off") == 0) {
			return;
		}
		if (train == nullptr || train[0] == '\0') {
			switch_requests = requested_switch();
		}
		map(name);
	}
	static uint64_t requested_switch() {
		const char *limit = std::getenv("ZEND_NATIVE_LAYOUT_REQUESTS");
		return limit != nullptr && limit[0] != '\0'
			? std::strtoull(limit, nullptr, 10) : layout_switch_requests;
	}
	/* Code compiled now counts its jumps. */
	bool active() const {
		return counters != nullptr && !switched;
	}
	/* The shared counters, and the file they go to (none: the profile
	 * stays in memory). */
	void map(const char *name) {
		void *memory = ::mmap(nullptr, capacity * sizeof(Counter),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (memory != MAP_FAILED) {
			counters = static_cast<Counter *>(memory);
			counters[0].key = 1;
			if (name != nullptr) {
				file = name;
				path = file.c_str();
			}
			timespec now{};
			::clock_gettime(CLOCK_REALTIME, &now);
			session = (static_cast<uint64_t>(
					reinterpret_cast<uintptr_t>(memory))
				^ (static_cast<uint64_t>(::getpid()) << 40)
				^ static_cast<uint64_t>(now.tv_nsec)
				^ (static_cast<uint64_t>(now.tv_sec) << 20)) | 1;
		}
	}
	Counter *allocate(uint64_t key, uint32_t ordinal) {
		const uint64_t index =
			__atomic_fetch_add(&counters[0].key, 1, __ATOMIC_RELAXED);
		if (index >= capacity) {
			return nullptr;
		}
		counters[index].key = key;
		counters[index].ordinal = ordinal;
		return &counters[index];
	}
	/* The profile of the counts so far. */
	void select(LayoutProfile &profile) const {
		const uint64_t used = std::min<uint64_t>(
			__atomic_load_n(&counters[0].key, __ATOMIC_RELAXED), capacity);
		LayoutProfile::Counts counts;
		for (uint64_t index = 1; index < used; ++index) {
			const Counter &counter = counters[index];
			auto &outcome = counts[counter.key][
				static_cast<uint32_t>(counter.ordinal)];
			outcome.first += counter.taken;
			outcome.second += counter.fallthrough;
		}
		counts[0][layout_requests_ordinal].first =
			__atomic_load_n(&counters[0].ordinal, __ATOMIC_RELAXED);
		profile.jumps.clear();
		profile.blocks.clear();
		profile.requests = 0;
		profile.all = false;
		profile.select(counts);
	}
	/* The counts so far replace the file, renamed into place. */
	void write() const {
		if (path == nullptr) {
			return;
		}
		const uint64_t used = std::min<uint64_t>(
			__atomic_load_n(&counters[0].key, __ATOMIC_RELAXED), capacity);
		const std::string name = std::string(path) + ".tmp"
			+ std::to_string(static_cast<long>(::getpid()));
		FILE *file = std::fopen(name.c_str(), "w");
		if (file == nullptr) {
			return;
		}
		std::fprintf(file, "0 %u %llu 0\n", layout_requests_ordinal,
			static_cast<unsigned long long>(__atomic_load_n(
				&counters[0].ordinal, __ATOMIC_RELAXED)));
		for (uint64_t index = 1; index < used; ++index) {
			const Counter &counter = counters[index];
			if (counter.taken + counter.fallthrough != 0) {
				std::fprintf(file, "%llx %u %llu %llu\n",
					static_cast<unsigned long long>(counter.key),
					static_cast<unsigned>(counter.ordinal),
					static_cast<unsigned long long>(counter.taken),
					static_cast<unsigned long long>(counter.fallthrough));
			}
		}
		std::fclose(file);
		std::rename(name.c_str(), path);
	}
	/* A process that exits normally (the CLI) writes the last counts,
	 * unless it switched to the profile already written. */
	~LayoutTraining() {
		if (path != nullptr && !switched
				&& __atomic_load_n(&counters[0].taken, __ATOMIC_ACQUIRE)
					== phase_training) {
			write();
		}
	}
};
extern LayoutTraining layout_training;

} // namespace zend::native::tpde
