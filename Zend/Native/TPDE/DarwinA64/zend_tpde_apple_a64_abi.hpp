// SPDX-License-Identifier: PHP-3.01
#pragma once

#include <cstddef>
#include <cstdint>

namespace zend::native::tpde {

/*
 * Apple ARM64 assignment for the fixed W06 entry ABI:
 *
 *   void entry(const scalar *arguments, uint64_t *slots, scalar *result)
 *
 * The three pointer arguments occupy x0-x2, so this wave has no stack or
 * variadic arguments. PHP scalar arguments live in the checked argument
 * array; they are not exposed as a platform-varargs call.
 */
class CCAssignerAppleA64 final {
public:
	static constexpr uint32_t argument_register_count = 3;
	static constexpr uint32_t first_argument_register = 0;
	static constexpr uint32_t last_argument_register = 2;
	static constexpr uint32_t platform_register_x18 = 18;
	static constexpr uint32_t frame_pointer_register = 29;
	static constexpr uint32_t link_register = 30;
	static constexpr size_t stack_alignment = 16;
	static constexpr bool supports_varargs = false;

	static constexpr bool allocatable(uint32_t reg) {
		return reg != platform_register_x18;
	}

	static constexpr bool valid_fixed_entry() {
		return argument_register_count == 3
			&& last_argument_register < platform_register_x18
			&& stack_alignment == 16 && !supports_varargs;
	}
};

static_assert(CCAssignerAppleA64::valid_fixed_entry());
static_assert(!CCAssignerAppleA64::allocatable(18));

} // namespace zend::native::tpde
