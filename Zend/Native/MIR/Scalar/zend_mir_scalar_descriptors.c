/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_scalar_descriptors.h"

#define SCALAR_FLAGS ZEND_MIR_VALUE_FACT_NON_REFCOUNTED
#define RANGE_FLAGS (SCALAR_FLAGS | ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE)
#define FINITE_FLAGS (SCALAR_FLAGS | ZEND_MIR_VALUE_FACT_FINITE)
#define REQ(rep, type, flags) \
	{ rep, type, flags, ZEND_MIR_OWNERSHIP_STATE_OWNED }
#define NONE_REQ REQ(ZEND_MIR_REPRESENTATION_VOID, ZEND_MIR_SCALAR_TYPE_NONE, 0)
#define I1_REQ REQ(ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_SCALAR_TYPE_I1, SCALAR_FLAGS)
#define I64_REQ REQ(ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_SCALAR_TYPE_I64, SCALAR_FLAGS)
#define I64_RANGE_REQ REQ(ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_SCALAR_TYPE_I64, RANGE_FLAGS)
#define F64_REQ REQ(ZEND_MIR_REPRESENTATION_DOUBLE, ZEND_MIR_SCALAR_TYPE_F64, SCALAR_FLAGS)
#define F64_FINITE_REQ REQ(ZEND_MIR_REPRESENTATION_DOUBLE, ZEND_MIR_SCALAR_TYPE_F64, FINITE_FLAGS)
#define DESCRIPTOR(op, text, count, a, b, result_present, result_req, proof) \
	{ op, text, count, { a, b }, result_present, result_req, proof, \
		0, 0, 0, 0, 0, true, false }

static const zend_mir_scalar_descriptor zend_mir_scalar_descriptors[] = {
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW, "i64_add_no_overflow", 2,
		I64_RANGE_REQ, I64_RANGE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW | ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW, "i64_sub_no_overflow", 2,
		I64_RANGE_REQ, I64_RANGE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW | ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW, "i64_mul_no_overflow", 2,
		I64_RANGE_REQ, I64_RANGE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW | ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_ADD, "f64_add", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, F64_FINITE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_SUB, "f64_sub", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, F64_FINITE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_MUL, "f64_mul", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, F64_FINITE_REQ,
		ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_MOD_NONZERO, "i64_mod_nonzero", 2,
		I64_RANGE_REQ,
		REQ(ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_SCALAR_TYPE_I64,
			RANGE_FLAGS | ZEND_MIR_VALUE_FACT_NONZERO),
		true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_NONZERO_DIVISOR
			| ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW
			| ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_SHL_CHECKED, "i64_shl_checked", 2,
		I64_RANGE_REQ, I64_RANGE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_VALID_SHIFT_COUNT
			| ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW
			| ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_SHR_CHECKED, "i64_shr_checked", 2,
		I64_RANGE_REQ, I64_RANGE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_VALID_SHIFT_COUNT
			| ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_BIT_OR, "i64_bit_or", 2,
		I64_REQ, I64_REQ, true, I64_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_BIT_AND, "i64_bit_and", 2,
		I64_REQ, I64_REQ, true, I64_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_BIT_XOR, "i64_bit_xor", 2,
		I64_REQ, I64_REQ, true, I64_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_BIT_NOT, "i64_bit_not", 1,
		I64_REQ, NONE_REQ, true, I64_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I1_NOT, "i1_not", 1,
		I1_REQ, NONE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I1_XOR, "i1_xor", 2,
		I1_REQ, I1_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_EQ, "i64_eq", 2,
		I64_REQ, I64_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_LT, "i64_lt", 2,
		I64_REQ, I64_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_LE, "i64_le", 2,
		I64_REQ, I64_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_CMP, "i64_cmp", 2,
		I64_REQ, I64_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_EQ, "f64_eq", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_LT, "f64_lt", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_LE, "f64_le", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_CMP, "f64_cmp", 2,
		F64_FINITE_REQ, F64_FINITE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I1_EQ, "i1_eq", 2,
		I1_REQ, I1_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_TO_F64, "i64_to_f64", 1,
		I64_REQ, NONE_REQ, true, F64_FINITE_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_TO_I64_CHECKED, "f64_to_i64_checked", 1,
		F64_FINITE_REQ, NONE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I64_TO_I1, "i64_to_i1", 1,
		I64_REQ, NONE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_F64_TO_I1, "f64_to_i1", 1,
		F64_FINITE_REQ, NONE_REQ, true, I1_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I1_TO_I64, "i1_to_i64", 1,
		I1_REQ, NONE_REQ, true, I64_RANGE_REQ,
		ZEND_MIR_SCALAR_PROOF_RESULT_RANGE),
	DESCRIPTOR(ZEND_MIR_OPCODE_I1_TO_F64, "i1_to_f64", 1,
		I1_REQ, NONE_REQ, true, F64_FINITE_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
	DESCRIPTOR(ZEND_MIR_OPCODE_SCALAR_DROP, "scalar_drop", 1,
		REQ(ZEND_MIR_REPRESENTATION_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
			SCALAR_FLAGS),
		NONE_REQ, false, NONE_REQ, ZEND_MIR_SCALAR_PROOF_NONE),
};

#undef DESCRIPTOR
#undef F64_FINITE_REQ
#undef F64_REQ
#undef I64_RANGE_REQ
#undef I64_REQ
#undef I1_REQ
#undef NONE_REQ
#undef REQ
#undef FINITE_FLAGS
#undef RANGE_FLAGS
#undef SCALAR_FLAGS

ZEND_MIR_STATIC_ASSERT(
	sizeof(zend_mir_scalar_descriptors) / sizeof(zend_mir_scalar_descriptors[0])
		== ZEND_MIR_SCALAR_OPCODE_COUNT,
	"every W03 scalar opcode has one descriptor");

const zend_mir_scalar_descriptor *zend_mir_scalar_descriptor_at(
		zend_mir_opcode opcode)
{
	switch (opcode) {
#define ZEND_MIR_SCALAR_DESCRIPTOR_CASE(symbol, label, value) \
		case ZEND_MIR_OPCODE_##symbol: \
			return &zend_mir_scalar_descriptors[ \
				(uint32_t) (value - ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW)];
		ZEND_MIR_SCALAR_OPCODE_CATALOG(ZEND_MIR_SCALAR_DESCRIPTOR_CASE)
#undef ZEND_MIR_SCALAR_DESCRIPTOR_CASE
		default:
			return NULL;
	}
}

bool zend_mir_scalar_opcode_is_registered(zend_mir_opcode opcode)
{
	const zend_mir_scalar_descriptor *descriptor =
		zend_mir_scalar_descriptor_at(opcode);

	return descriptor != NULL && descriptor->opcode == opcode;
}

zend_mir_representation zend_mir_scalar_type_representation(
		zend_mir_scalar_type_mask type)
{
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			return ZEND_MIR_REPRESENTATION_ZVAL;
		case ZEND_MIR_SCALAR_TYPE_I1:
			return ZEND_MIR_REPRESENTATION_I1;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return ZEND_MIR_REPRESENTATION_I64;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return ZEND_MIR_REPRESENTATION_DOUBLE;
		default:
			return ZEND_MIR_REPRESENTATION_INVALID;
	}
}

bool zend_mir_scalar_fact_is_well_formed(const zend_mir_value_fact_ref *fact)
{
	const zend_mir_value_fact_flags known_flags =
		ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NONZERO
		| ZEND_MIR_VALUE_FACT_FINITE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;

	if (fact == NULL || !zend_mir_id_is_valid(fact->value_id)
			|| !zend_mir_scalar_type_is_exact(fact->exact_type)
			|| (fact->flags & ~known_flags) != 0
			|| fact->provenance < 0
			|| fact->provenance >= ZEND_MIR_FACT_PROVENANCE_COUNT) {
		return false;
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0) {
		if (fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| fact->integer_min > fact->integer_max) {
			return false;
		}
	} else if (fact->integer_min != 0 || fact->integer_max != 0) {
		return false;
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_NONZERO) != 0) {
		if (fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| ((fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
					&& fact->integer_min <= 0 && fact->integer_max >= 0)) {
			return false;
		}
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_FINITE) != 0
			&& fact->exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
		return false;
	}
	return true;
}
