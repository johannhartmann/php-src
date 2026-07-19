/* ZNMIR core opcode and representation contract. */

#ifndef ZEND_MIR_OPCODES_H
#define ZEND_MIR_OPCODES_H

#include <stdint.h>

#include "zend_mir_ids.h"

#define ZEND_MIR_OPCODE_CATALOG(X) \
	X(CONSTANT, "constant", 0) \
	X(PHI, "phi", 1) \
	X(COPY, "copy", 2) \
	X(CANONICALIZE, "canonicalize", 3) \
	X(STATEPOINT, "statepoint", 4) \
	X(BRANCH, "branch", 5) \
	X(COND_BRANCH, "cond_branch", 6) \
	X(RETURN, "return", 7) \
	X(THROW, "throw", 8) \
	X(UNREACHABLE, "unreachable", 9)

#define ZEND_MIR_CALL_OPCODE_CATALOG(X) \
	X(CALL_DIRECT_USER, "call_direct_user", 41)

#define ZEND_MIR_SCALAR_OPCODE_CATALOG(X) \
	X(I64_ADD_NO_OVERFLOW, "i64_add_no_overflow", 10) \
	X(I64_SUB_NO_OVERFLOW, "i64_sub_no_overflow", 11) \
	X(I64_MUL_NO_OVERFLOW, "i64_mul_no_overflow", 12) \
	X(F64_ADD, "f64_add", 13) \
	X(F64_SUB, "f64_sub", 14) \
	X(F64_MUL, "f64_mul", 15) \
	X(I64_MOD_NONZERO, "i64_mod_nonzero", 16) \
	X(I64_SHL_CHECKED, "i64_shl_checked", 17) \
	X(I64_SHR_CHECKED, "i64_shr_checked", 18) \
	X(I64_BIT_OR, "i64_bit_or", 19) \
	X(I64_BIT_AND, "i64_bit_and", 20) \
	X(I64_BIT_XOR, "i64_bit_xor", 21) \
	X(I64_BIT_NOT, "i64_bit_not", 22) \
	X(I1_NOT, "i1_not", 23) \
	X(I1_XOR, "i1_xor", 24) \
	X(I64_EQ, "i64_eq", 25) \
	X(I64_LT, "i64_lt", 26) \
	X(I64_LE, "i64_le", 27) \
	X(I64_CMP, "i64_cmp", 28) \
	X(F64_EQ, "f64_eq", 29) \
	X(F64_LT, "f64_lt", 30) \
	X(F64_LE, "f64_le", 31) \
	X(F64_CMP, "f64_cmp", 32) \
	X(I1_EQ, "i1_eq", 33) \
	X(I64_TO_F64, "i64_to_f64", 34) \
	X(F64_TO_I64_CHECKED, "f64_to_i64_checked", 35) \
	X(I64_TO_I1, "i64_to_i1", 36) \
	X(F64_TO_I1, "f64_to_i1", 37) \
	X(I1_TO_I64, "i1_to_i64", 38) \
	X(I1_TO_F64, "i1_to_f64", 39) \
	X(SCALAR_DROP, "scalar_drop", 40)

#define ZEND_MIR_OPCODE_ENUM(symbol, label, value) ZEND_MIR_OPCODE_##symbol = value,
typedef enum _zend_mir_opcode {
	ZEND_MIR_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_SCALAR_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_CALL_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	/*
	 * Keep the W03 scalar range boundary stable. W05 is modeling-only and
	 * publishes its additive table boundary separately.
	 */
	ZEND_MIR_OPCODE_COUNT = 41,
	ZEND_MIR_W05_OPCODE_COUNT = 42,
	ZEND_MIR_OPCODE_INVALID = -1
} zend_mir_opcode;
#undef ZEND_MIR_OPCODE_ENUM

#define ZEND_MIR_REPRESENTATION_CATALOG(X) \
	X(VOID, "void", 0) \
	X(CONTROL, "control", 1) \
	X(I1, "i1", 2) \
	X(I8, "i8", 3) \
	X(I16, "i16", 4) \
	X(I32, "i32", 5) \
	X(I64, "i64", 6) \
	X(DOUBLE, "double", 7) \
	X(SEMANTIC_POINTER, "semantic_pointer", 8) \
	X(ZVAL, "zval", 9)

#define ZEND_MIR_REPRESENTATION_ENUM(symbol, label, value) ZEND_MIR_REPRESENTATION_##symbol = value,
typedef enum _zend_mir_representation {
	ZEND_MIR_REPRESENTATION_CATALOG(ZEND_MIR_REPRESENTATION_ENUM)
	ZEND_MIR_REPRESENTATION_COUNT = 10,
	ZEND_MIR_REPRESENTATION_INVALID = -1
} zend_mir_representation;
#undef ZEND_MIR_REPRESENTATION_ENUM

#define ZEND_MIR_CONSTANT_KIND_CATALOG(X) \
	X(SIGNED_INTEGER_BITS, "signed_integer_bits", 0) \
	X(DOUBLE_BITS, "double_bits", 1) \
	X(NULL_VALUE, "null", 2) \
	X(FALSE_VALUE, "false", 3) \
	X(TRUE_VALUE, "true", 4) \
	X(STRING_SYMBOL, "string_symbol", 5) \
	X(SEMANTIC_POINTER_SYMBOL, "semantic_pointer_symbol", 6)

#define ZEND_MIR_CONSTANT_KIND_ENUM(symbol, label, value) ZEND_MIR_CONSTANT_KIND_##symbol = value,
typedef enum _zend_mir_constant_kind {
	ZEND_MIR_CONSTANT_KIND_CATALOG(ZEND_MIR_CONSTANT_KIND_ENUM)
	ZEND_MIR_CONSTANT_KIND_COUNT = 7,
	ZEND_MIR_CONSTANT_KIND_INVALID = -1
} zend_mir_constant_kind;
#undef ZEND_MIR_CONSTANT_KIND_ENUM

static inline bool zend_mir_opcode_is_terminator(zend_mir_opcode opcode)
{
	return opcode == ZEND_MIR_OPCODE_BRANCH
		|| opcode == ZEND_MIR_OPCODE_COND_BRANCH
		|| opcode == ZEND_MIR_OPCODE_RETURN
		|| opcode == ZEND_MIR_OPCODE_THROW
		|| opcode == ZEND_MIR_OPCODE_UNREACHABLE;
}

ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_COUNT < UINT32_MAX,
	"opcode invalid value remains unique");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_CALL_DIRECT_USER == ZEND_MIR_OPCODE_COUNT,
	"W05 call opcode begins after the frozen W03 scalar range");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W05_OPCODE_COUNT == ZEND_MIR_OPCODE_CALL_DIRECT_USER + 1,
	"W05 call opcode has an additive table boundary");

#endif /* ZEND_MIR_OPCODES_H */
