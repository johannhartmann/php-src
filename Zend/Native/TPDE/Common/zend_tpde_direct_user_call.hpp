// SPDX-License-Identifier: PHP-3.01
#pragma once

#include <cstdint>

/*
 * The operand layout of a CALL_DIRECT_USER node. The adaptor emits, after
 * the argument operands, one frame operand per use of the caller's frame
 * and then one context operand per use of the execution context, so that
 * TPDE's reference counts match every path the backends compile; the
 * backends find each use by its index.
 */

/*
 * The frame uses of a call through a generated (inline) frame: a leaf
 * scalar frame's three, else six, one more for a machine result and two
 * more for a variadic callee.
 */
constexpr uint32_t zend_tpde_direct_user_call_frame_uses(
		bool leaf_scalar_frame, bool has_result, bool variadic_frame) {
	return leaf_scalar_frame
		? 3
		: 6 + has_result + (variadic_frame ? 2 : 0);
}

/*
 * The context uses of a direct call: a generated leaf scalar frame's five,
 * one more for a machine result unless its body is inlined; seven for
 * another generated frame; three otherwise.
 */
constexpr uint32_t zend_tpde_direct_user_call_context_uses(
		bool generated_frame, bool leaf_scalar_frame, bool inlined_body,
		bool has_result) {
	return generated_frame
		? leaf_scalar_frame
			? inlined_body
				? 5
				: 5 + has_result
			: 7
		: 3;
}

/*
 * Where a CALL_DIRECT_USER node's frame and context operands start, and
 * the index of each use the slow path takes among them: entering and
 * leaving the canonical call, and its entry.
 */
struct zend_tpde_direct_user_call_layout {
	uint32_t frame_operand;
	uint32_t context_operand;
	uint32_t slow_enter_frame_use;
	uint32_t slow_enter_context_use;
	uint32_t slow_entry_context_use;
	uint32_t slow_leave_frame_use;
	uint32_t slow_leave_context_use;
};

/*
 * The layout of a node compiled as a split guard's cold block (split_cold),
 * as a typed body call, through a generated frame (private_inline_body for
 * a leaf scalar frame) or through the canonical call.
 */
constexpr zend_tpde_direct_user_call_layout
zend_tpde_direct_user_call_layout_of(bool split_cold, bool typed_body,
		bool generated_fast_path, bool private_inline_body,
		bool variadic_frame, bool has_result, uint32_t argument_count) {
	const uint32_t frame_operand = split_cold
		? 0
		: typed_body
			? argument_count
			: generated_fast_path ? argument_count : 0;
	const uint32_t frame_use_count =
			split_cold ? 1
				: typed_body ? 2
				: generated_fast_path
					? zend_tpde_direct_user_call_frame_uses(
						private_inline_body, has_result, variadic_frame)
					: 2;
	return {
		.frame_operand = frame_operand,
		.context_operand = frame_operand + frame_use_count,
		.slow_enter_frame_use =
			generated_fast_path ? (private_inline_body ? 1u : 4u) : 0u,
		.slow_enter_context_use =
			generated_fast_path ? (private_inline_body ? 2u : 4u) : 0u,
		.slow_entry_context_use =
			generated_fast_path ? (private_inline_body ? 3u : 5u) : 1u,
		.slow_leave_frame_use =
			generated_fast_path ? (private_inline_body ? 2u : 5u) : 1u,
		.slow_leave_context_use =
			generated_fast_path ? (private_inline_body ? 4u : 6u) : 2u,
	};
}
