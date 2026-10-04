#include "zend_mir_zend_source_internal.h"

static bool zend_mir_frontend_pointer_add(
	uintptr_t base, int32_t offset, uintptr_t *out)
{
	if (out == NULL) {
		return false;
	}
	if (offset >= 0) {
		if (base > UINTPTR_MAX - (uintptr_t) offset) {
			return false;
		}
		*out = base + (uintptr_t) offset;
	} else {
		uintptr_t magnitude = (uintptr_t) (-(int64_t) offset);

		if (base < magnitude) {
			return false;
		}
		*out = base - magnitude;
	}
	return true;
}

bool zend_mir_frontend_literal_index(
	const zend_op_array *op_array,
	const zend_op *opline,
	const znode_op *node,
	uint32_t *literal_index)
{
	uintptr_t literal_address;
	uintptr_t literal_base;
	uintptr_t literal_end;
	uintptr_t byte_offset;
	uintptr_t literal_bytes;

	if (op_array == NULL || opline == NULL || node == NULL
			|| literal_index == NULL || op_array->last_literal == 0
			|| op_array->literals == NULL) {
		return false;
	}

	if ((op_array->fn_flags & ZEND_ACC_DONE_PASS_TWO) == 0) {
		if (node->constant >= op_array->last_literal) {
			return false;
		}
		*literal_index = node->constant;
		return true;
	}

#if ZEND_USE_ABS_CONST_ADDR
	literal_address = (uintptr_t) node->zv;
#else
	if (!zend_mir_frontend_pointer_add(
			(uintptr_t) opline, (int32_t) node->constant, &literal_address)) {
		return false;
	}
#endif

	literal_base = (uintptr_t) op_array->literals;
	literal_bytes = (uintptr_t) op_array->last_literal * sizeof(zval);
	if (literal_bytes / sizeof(zval) != op_array->last_literal
			|| literal_base > UINTPTR_MAX - literal_bytes) {
		return false;
	}
	literal_end = literal_base + literal_bytes;
	if (literal_address < literal_base || literal_address >= literal_end) {
		return false;
	}
	byte_offset = literal_address - literal_base;
	if (byte_offset % sizeof(zval) != 0) {
		return false;
	}
	*literal_index = (uint32_t) (byte_offset / sizeof(zval));
	return true;
}

static bool zend_mir_frontend_canonical_literal(
	const zval *literal, uint32_t index, zend_mir_source_literal_ref *out)
{
	uint64_t bits = 0;

	if (literal == NULL || out == NULL) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->literal_index = index;
	switch (Z_TYPE_P(literal)) {
		case IS_NULL:
			out->kind = ZEND_MIR_SOURCE_LITERAL_NULL;
			break;
		case IS_FALSE:
			out->kind = ZEND_MIR_SOURCE_LITERAL_FALSE;
			break;
		case IS_TRUE:
			out->kind = ZEND_MIR_SOURCE_LITERAL_TRUE;
			break;
		case IS_LONG:
			bits = (uint64_t) (int64_t) Z_LVAL_P(literal);
			out->kind = ZEND_MIR_SOURCE_LITERAL_LONG_BITS;
			break;
		case IS_DOUBLE:
			memcpy(&bits, &Z_DVAL_P(literal), sizeof(bits));
			out->kind = ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS;
			break;
		default:
			return false;
	}
	out->payload_bits = bits;
	out->flags = ZEND_MIR_SOURCE_LITERAL_CANONICAL_SCALAR;
	return true;
}

bool zend_mir_frontend_canonical_literal_for_index(
	const zend_op_array *op_array,
	uint32_t index,
	zend_mir_source_literal_ref *out)
{
	return op_array != NULL && out != NULL && index < op_array->last_literal
		&& op_array->literals != NULL
		&& zend_mir_frontend_canonical_literal(
			&op_array->literals[index], index, out);
}

bool zend_mir_frontend_literal_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_literal_ref *out)
{
	const zend_op_array *op_array;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->literal_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	return zend_mir_frontend_canonical_literal_for_index(
		op_array, index, out);
}
