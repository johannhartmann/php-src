/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <limits.h>

#include "zend_mir_lower_numeric.h"

static bool zend_mir_numeric_range_is_valid(zend_mir_numeric_range range)
{
	return range.minimum <= range.maximum;
}

static int64_t zend_mir_numeric_minimum4(
	int64_t first, int64_t second, int64_t third, int64_t fourth)
{
	int64_t result = first < second ? first : second;

	result = result < third ? result : third;
	return result < fourth ? result : fourth;
}

static int64_t zend_mir_numeric_maximum4(
	int64_t first, int64_t second, int64_t third, int64_t fourth)
{
	int64_t result = first > second ? first : second;

	result = result > third ? result : third;
	return result > fourth ? result : fourth;
}

bool zend_mir_numeric_range_add(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result)
{
	if (result == NULL || !zend_mir_numeric_range_is_valid(left)
			|| !zend_mir_numeric_range_is_valid(right)
			|| (right.minimum < 0
				&& left.minimum < INT64_MIN - right.minimum)
			|| (right.maximum > 0
				&& left.maximum > INT64_MAX - right.maximum)) {
		return false;
	}
	result->minimum = left.minimum + right.minimum;
	result->maximum = left.maximum + right.maximum;
	return true;
}

bool zend_mir_numeric_range_subtract(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result)
{
	if (result == NULL || !zend_mir_numeric_range_is_valid(left)
			|| !zend_mir_numeric_range_is_valid(right)
			|| (right.maximum > 0
				&& left.minimum < INT64_MIN + right.maximum)
			|| (right.minimum < 0
				&& left.maximum > INT64_MAX + right.minimum)) {
		return false;
	}
	result->minimum = left.minimum - right.maximum;
	result->maximum = left.maximum - right.minimum;
	return true;
}

static bool zend_mir_numeric_multiply_pair(
	int64_t left, int64_t right, int64_t *result)
{
	if (result == NULL) {
		return false;
	}
	if (left > 0) {
		if ((right > 0 && left > INT64_MAX / right)
				|| (right < 0 && right < INT64_MIN / left)) {
			return false;
		}
	} else if (left < 0) {
		if ((right > 0 && left < INT64_MIN / right)
				|| (right < 0 && left < INT64_MAX / right)) {
			return false;
		}
	}
	*result = left * right;
	return true;
}

bool zend_mir_numeric_range_multiply(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result)
{
	int64_t products[4];

	if (result == NULL || !zend_mir_numeric_range_is_valid(left)
			|| !zend_mir_numeric_range_is_valid(right)
			|| !zend_mir_numeric_multiply_pair(
				left.minimum, right.minimum, &products[0])
			|| !zend_mir_numeric_multiply_pair(
				left.minimum, right.maximum, &products[1])
			|| !zend_mir_numeric_multiply_pair(
				left.maximum, right.minimum, &products[2])
			|| !zend_mir_numeric_multiply_pair(
				left.maximum, right.maximum, &products[3])) {
		return false;
	}
	result->minimum = zend_mir_numeric_minimum4(
		products[0], products[1], products[2], products[3]);
	result->maximum = zend_mir_numeric_maximum4(
		products[0], products[1], products[2], products[3]);
	return true;
}

bool zend_mir_numeric_range_modulo(
	zend_mir_numeric_range dividend, zend_mir_numeric_range divisor,
	zend_mir_numeric_range *result)
{
	uint64_t minimum_magnitude;
	uint64_t maximum_magnitude;
	uint64_t divisor_magnitude;
	int64_t remainder_magnitude;

	if (!zend_mir_numeric_range_is_valid(dividend)
			|| !zend_mir_numeric_range_is_valid(divisor)
			|| result == NULL
			|| (divisor.minimum <= 0 && divisor.maximum >= 0)
			|| (dividend.minimum == INT64_MIN
				&& divisor.minimum <= -1 && divisor.maximum >= -1)) {
		return false;
	}
	minimum_magnitude = divisor.minimum == INT64_MIN
		? UINT64_C(1) << 63
		: (uint64_t) (divisor.minimum < 0
			? -divisor.minimum : divisor.minimum);
	maximum_magnitude = divisor.maximum == INT64_MIN
		? UINT64_C(1) << 63
		: (uint64_t) (divisor.maximum < 0
			? -divisor.maximum : divisor.maximum);
	divisor_magnitude = minimum_magnitude > maximum_magnitude
		? minimum_magnitude : maximum_magnitude;
	remainder_magnitude = divisor_magnitude > (uint64_t) INT64_MAX
		? INT64_MAX : (int64_t) divisor_magnitude - 1;
	result->minimum = dividend.minimum >= 0 ? 0 : -remainder_magnitude;
	result->maximum = dividend.maximum <= 0 ? 0 : remainder_magnitude;
	return true;
}

bool zend_mir_numeric_modulo_is_safe(
	zend_mir_numeric_range dividend, zend_mir_numeric_range divisor)
{
	zend_mir_numeric_range result;

	return zend_mir_numeric_range_modulo(dividend, divisor, &result);
}

static bool zend_mir_numeric_power_of_two(
	uint32_t count, int64_t *factor_out)
{
	uint64_t factor;

	if (factor_out == NULL || count > 62) {
		return false;
	}
	factor = UINT64_C(1) << count;
	*factor_out = (int64_t) factor;
	return true;
}

bool zend_mir_numeric_shift_left(
	zend_mir_numeric_range value, zend_mir_numeric_range count,
	zend_mir_numeric_range *result)
{
	zend_mir_numeric_range factor;

	if (result == NULL || !zend_mir_numeric_range_is_valid(value)
			|| !zend_mir_numeric_range_is_valid(count)
			|| count.minimum < 0 || count.maximum > 63) {
		return false;
	}
	if (count.maximum == 63) {
		if (value.minimum < -1 || value.maximum > 0) {
			return false;
		}
		result->minimum = value.minimum == -1 ? INT64_MIN : 0;
		result->maximum = 0;
		return true;
	}
	if (!zend_mir_numeric_power_of_two(
			(uint32_t) count.minimum, &factor.minimum)
			|| !zend_mir_numeric_power_of_two(
				(uint32_t) count.maximum, &factor.maximum)) {
		return false;
	}
	return zend_mir_numeric_range_multiply(value, factor, result);
}

static int64_t zend_mir_numeric_arithmetic_shift_right(
	int64_t value, uint32_t count)
{
	if (count == 0) {
		return value;
	}
	if (value >= 0) {
		return (int64_t) ((uint64_t) value >> count);
	}
	return -INT64_C(1)
		- (int64_t) ((uint64_t) (-(value + INT64_C(1))) >> count);
}

bool zend_mir_numeric_shift_right(
	zend_mir_numeric_range value, zend_mir_numeric_range count,
	zend_mir_numeric_range *result)
{
	int64_t shifted[4];

	if (result == NULL || !zend_mir_numeric_range_is_valid(value)
			|| !zend_mir_numeric_range_is_valid(count)
			|| count.minimum < 0 || count.maximum > 63) {
		return false;
	}
	shifted[0] = zend_mir_numeric_arithmetic_shift_right(
		value.minimum, (uint32_t) count.minimum);
	shifted[1] = zend_mir_numeric_arithmetic_shift_right(
		value.minimum, (uint32_t) count.maximum);
	shifted[2] = zend_mir_numeric_arithmetic_shift_right(
		value.maximum, (uint32_t) count.minimum);
	shifted[3] = zend_mir_numeric_arithmetic_shift_right(
		value.maximum, (uint32_t) count.maximum);
	result->minimum = zend_mir_numeric_minimum4(
		shifted[0], shifted[1], shifted[2], shifted[3]);
	result->maximum = zend_mir_numeric_maximum4(
		shifted[0], shifted[1], shifted[2], shifted[3]);
	return true;
}
