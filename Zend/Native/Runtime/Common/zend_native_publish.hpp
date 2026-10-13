// SPDX-License-Identifier: PHP-3.01
#pragma once

/* The parts of the image publishers (Runtime/<target>) every target shares. */

#include <cstddef>
#include <cstdint>
#include <cstring>

/*
 * The component a defined entry symbol names: zend_native_entry is
 * component 0, zend_native_component_<index> (canonical decimal, as the
 * adaptor names them) the others; UINT32_MAX for any other name.
 */
inline uint32_t zend_native_component_entry_index(
	const char *name, size_t length, uint32_t count)
{
	static constexpr char entry[] = "zend_native_entry";
	static constexpr char prefix[] = "zend_native_component_";
	constexpr size_t prefix_length = sizeof(prefix) - 1;
	uint64_t index = 0;

	if (length == sizeof(entry) - 1 && std::memcmp(name, entry, length) == 0) {
		return count != 0 ? 0 : UINT32_MAX;
	}
	if (length <= prefix_length || length > prefix_length + 10
			|| std::memcmp(name, prefix, prefix_length) != 0
			|| name[prefix_length] == '0') {
		return UINT32_MAX;
	}
	for (size_t at = prefix_length; at < length; ++at) {
		if (name[at] < '0' || name[at] > '9') {
			return UINT32_MAX;
		}
		index = index * 10 + static_cast<uint64_t>(name[at] - '0');
	}
	return index < count ? static_cast<uint32_t>(index) : UINT32_MAX;
}

#if (defined(__linux__) && defined(__x86_64__)) \
	|| (defined(__APPLE__) && defined(__aarch64__))
# if defined(__APPLE__)
#  define ZEND_NATIVE_UNWIND_WEAK __attribute__((weak_import))
# else
#  define ZEND_NATIVE_UNWIND_WEAK __attribute__((weak))
# endif
extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);
extern "C" void __unw_add_dynamic_eh_frame_section(uintptr_t)
	ZEND_NATIVE_UNWIND_WEAK;
extern "C" void __unw_remove_dynamic_eh_frame_section(uintptr_t)
	ZEND_NATIVE_UNWIND_WEAK;

/*
 * Registers an image's .eh_frame section with the unwinder: as one dynamic
 * section where the unwinder takes those (LLVM libunwind), else through
 * each_frame(__register_frame), which passes the section (libgcc) or each
 * of its FDEs (Darwin's libunwind) to it.
 */
template <typename EachFrame>
void zend_native_register_eh_frame(void *section, EachFrame each_frame) {
	if (__unw_add_dynamic_eh_frame_section != nullptr) {
		__unw_add_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(section));
	} else {
		each_frame(__register_frame);
	}
}

/* Undoes zend_native_register_eh_frame() of the same section. */
template <typename EachFrame>
void zend_native_deregister_eh_frame(void *section, EachFrame each_frame) {
	if (__unw_remove_dynamic_eh_frame_section != nullptr) {
		__unw_remove_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(section));
	} else {
		each_frame(__deregister_frame);
	}
}
#endif
