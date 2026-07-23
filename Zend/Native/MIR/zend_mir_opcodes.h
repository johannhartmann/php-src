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
	X(CALL_DIRECT_USER, "call_direct_user", 41) \
	X(CALL_DIRECT_INTERNAL, "call_direct_internal", 48) \
	X(CATCH_ENTER, "catch_enter", 49) \
	X(FINALLY_ENTER, "finally_enter", 50) \
	X(FINALLY_CALL, "finally_call", 51) \
	X(FINALLY_RETURN, "finally_return", 52) \
	X(RETURN_SOURCE_ZVAL, "return_source_zval", 53)

#define ZEND_MIR_VALUE_OPCODE_CATALOG(X) \
	X(STORAGE_BIND, "storage_bind", 42) \
	X(REFERENCE_BIND, "reference_bind", 43) \
	X(INDIRECT_BIND, "indirect_bind", 44) \
	X(OWNERSHIP_TRANSFER, "ownership_transfer", 45) \
	X(ALIAS_RELATION, "alias_relation", 46) \
	X(SEPARATION_PLAN, "separation_plan", 47)

#define ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG(X) \
	X(VALUE_MAKE_REF, "value_make_ref", 54) \
	X(VALUE_ASSIGN_REF, "value_assign_ref", 55) \
	X(VALUE_SEPARATE, "value_separate", 56) \
	X(VALUE_COPY_TMP, "value_copy_tmp", 57) \
	X(VALUE_FREE, "value_free", 58) \
	X(VALUE_UNSET_CV, "value_unset_cv", 59) \
	X(VALUE_CHECK_VAR, "value_check_var", 60) \
	X(VALUE_ASSIGN, "value_assign", 61) \
	X(VALUE_QM_ASSIGN, "value_qm_assign", 62) \
	X(VALUE_CONCAT, "value_concat", 63) \
	X(VALUE_FAST_CONCAT, "value_fast_concat", 64) \
	X(VALUE_ROPE_INIT, "value_rope_init", 65) \
	X(VALUE_ROPE_ADD, "value_rope_add", 66) \
	X(VALUE_ROPE_END, "value_rope_end", 67) \
	X(VALUE_INIT_ARRAY, "value_init_array", 68) \
	X(VALUE_ADD_ARRAY_ELEMENT, "value_add_array_element", 69) \
	X(VALUE_ADD_ARRAY_UNPACK, "value_add_array_unpack", 70) \
	X(VALUE_FETCH_DIM_R, "value_fetch_dim_r", 71) \
	X(VALUE_FETCH_DIM_W, "value_fetch_dim_w", 72) \
	X(VALUE_FETCH_DIM_RW, "value_fetch_dim_rw", 73) \
	X(VALUE_FETCH_DIM_IS, "value_fetch_dim_is", 74) \
	X(VALUE_FETCH_DIM_FUNC_ARG, "value_fetch_dim_func_arg", 75) \
	X(VALUE_FETCH_DIM_UNSET, "value_fetch_dim_unset", 76) \
	X(VALUE_ASSIGN_DIM, "value_assign_dim", 77) \
	X(VALUE_ASSIGN_DIM_OP, "value_assign_dim_op", 78) \
	X(VALUE_UNSET_DIM, "value_unset_dim", 79) \
	X(VALUE_ISSET_ISEMPTY_DIM, "value_isset_isempty_dim", 80) \
	X(VALUE_ASSIGN_OP, "value_assign_op", 82) \
	X(VALUE_FE_FREE, "value_fe_free", 83) \
	X(VALUE_BINARY_OP, "value_binary_op", 84) \
	X(VALUE_UNARY_OP, "value_unary_op", 85) \
	X(VALUE_CAST, "value_cast", 86) \
	X(VALUE_ISSET_ISEMPTY_CV, "value_isset_isempty_cv", 87) \
	X(VALUE_FETCH_LIST, "value_fetch_list", 88) \
	X(VALUE_INCDEC, "value_incdec", 90)

#define ZEND_MIR_ITERATOR_OPCODE_CATALOG(X) \
	X(ITERATOR_BRANCH, "iterator_branch", 81) \
	X(VALUE_COND_BRANCH, "value_cond_branch", 89)

#define ZEND_MIR_OBJECT_OPCODE_CATALOG(X) \
	X(OBJECT_DECLARE_ANON_CLASS, "object_declare_anon_class", 91) \
	X(OBJECT_FETCH_THIS, "object_fetch_this", 92) \
	X(OBJECT_FETCH_R, "object_fetch_r", 93) \
	X(OBJECT_FETCH_W, "object_fetch_w", 94) \
	X(OBJECT_FETCH_RW, "object_fetch_rw", 95) \
	X(OBJECT_FETCH_IS, "object_fetch_is", 96) \
	X(OBJECT_FETCH_FUNC_ARG, "object_fetch_func_arg", 97) \
	X(OBJECT_FETCH_UNSET, "object_fetch_unset", 98) \
	X(OBJECT_ASSIGN, "object_assign", 99) \
	X(OBJECT_ASSIGN_REF, "object_assign_ref", 100) \
	X(OBJECT_ASSIGN_OP, "object_assign_op", 101) \
	X(OBJECT_UNSET, "object_unset", 102) \
	X(OBJECT_ISSET_ISEMPTY, "object_isset_isempty", 103) \
	X(OBJECT_PRE_INC, "object_pre_inc", 104) \
	X(OBJECT_PRE_DEC, "object_pre_dec", 105) \
	X(OBJECT_POST_INC, "object_post_inc", 106) \
	X(OBJECT_POST_DEC, "object_post_dec", 107) \
	X(OBJECT_INSTANCEOF, "object_instanceof", 108) \
	X(OBJECT_CLONE, "object_clone", 109) \
	X(STATIC_FETCH_R, "static_fetch_r", 110) \
	X(STATIC_FETCH_W, "static_fetch_w", 111) \
	X(STATIC_FETCH_RW, "static_fetch_rw", 112) \
	X(STATIC_FETCH_IS, "static_fetch_is", 113) \
	X(STATIC_FETCH_FUNC_ARG, "static_fetch_func_arg", 114) \
	X(STATIC_FETCH_UNSET, "static_fetch_unset", 115) \
	X(STATIC_ASSIGN, "static_assign", 116) \
	X(STATIC_ASSIGN_REF, "static_assign_ref", 117) \
	X(STATIC_ASSIGN_OP, "static_assign_op", 118) \
	X(STATIC_PRE_INC, "static_pre_inc", 119) \
	X(STATIC_PRE_DEC, "static_pre_dec", 120) \
	X(STATIC_POST_INC, "static_post_inc", 121) \
	X(STATIC_POST_DEC, "static_post_dec", 122) \
	X(STATIC_ISSET_ISEMPTY, "static_isset_isempty", 123) \
	X(STATIC_UNSET, "static_unset", 124) \
	X(OBJECT_FETCH_CLASS, "object_fetch_class", 125) \
	X(OBJECT_FETCH_CLASS_CONSTANT, "object_fetch_class_constant", 126) \
	X(OBJECT_DECLARE_LAMBDA, "object_declare_lambda", 127) \
	X(OBJECT_BIND_LEXICAL, "object_bind_lexical", 128) \
	X(OBJECT_BIND_STATIC, "object_bind_static", 129) \
	X(THROW_SOURCE_ZVAL, "throw_source_zval", 130) \
	X(VALUE_TYPE_CHECK, "value_type_check", 131) \
	X(CALL_FRAMELESS_INTERNAL, "call_frameless_internal", 132) \
	X(OBJECT_FETCH_CLASS_NAME, "object_fetch_class_name", 133) \
	X(OBJECT_DECLARE_FUNCTION, "object_declare_function", 134) \
	X(OBJECT_DECLARE_CLASS, "object_declare_class", 135) \
	X(OBJECT_DECLARE_CLASS_DELAYED, "object_declare_class_delayed", 136)

#define ZEND_MIR_DYNAMIC_OPCODE_CATALOG(X) \
	X(DYNAMIC_FETCH_R, "dynamic_fetch_r", 137) \
	X(DYNAMIC_FETCH_W, "dynamic_fetch_w", 138) \
	X(DYNAMIC_FETCH_RW, "dynamic_fetch_rw", 139) \
	X(DYNAMIC_FETCH_IS, "dynamic_fetch_is", 140) \
	X(DYNAMIC_FETCH_FUNC_ARG, "dynamic_fetch_func_arg", 141) \
	X(DYNAMIC_FETCH_UNSET, "dynamic_fetch_unset", 142) \
	X(DYNAMIC_UNSET_VAR, "dynamic_unset_var", 143) \
	X(DYNAMIC_ISSET_ISEMPTY_VAR, "dynamic_isset_isempty_var", 144) \
	X(DYNAMIC_BIND_GLOBAL, "dynamic_bind_global", 145) \
	X(DYNAMIC_FETCH_GLOBALS, "dynamic_fetch_globals", 146) \
	X(DYNAMIC_FETCH_CONSTANT, "dynamic_fetch_constant", 147) \
	X(DYNAMIC_DECLARE_CONSTANT, "dynamic_declare_constant", 148) \
	X(DYNAMIC_DECLARE_ATTRIBUTED_CONSTANT, "dynamic_declare_attributed_constant", 149) \
	X(DYNAMIC_INCLUDE_OR_EVAL, "dynamic_include_or_eval", 150)

#define ZEND_MIR_W11P_OPCODE_CATALOG(X) \
	X(ECHO_SCALAR, "echo_scalar", 151) \
	X(VERIFY_RETURN_TYPE, "verify_return_type", 152)

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
	ZEND_MIR_VALUE_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_ITERATOR_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_OBJECT_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_DYNAMIC_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_W11P_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	/*
	 * Keep the W03 scalar range boundary stable. W05 is modeling-only and
	 * publishes its additive table boundary separately.
	 */
	ZEND_MIR_OPCODE_COUNT = 41,
	ZEND_MIR_W05_OPCODE_COUNT = 42,
	ZEND_MIR_W06_OPCODE_COUNT = 48,
	ZEND_MIR_W08_OPCODE_COUNT = 54,
	ZEND_MIR_W09_OPCODE_COUNT = 91,
	ZEND_MIR_W10_OPCODE_COUNT = 137,
	ZEND_MIR_W11_OPCODE_COUNT = 151,
	ZEND_MIR_W11P_OPCODE_COUNT = 153,
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
		|| opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
		|| opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH
		|| opcode == ZEND_MIR_OPCODE_CATCH_ENTER
		|| opcode == ZEND_MIR_OPCODE_FINALLY_CALL
		|| opcode == ZEND_MIR_OPCODE_FINALLY_RETURN
		|| opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
		|| opcode == ZEND_MIR_OPCODE_RETURN
		|| opcode == ZEND_MIR_OPCODE_THROW
		|| opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
		|| opcode == ZEND_MIR_OPCODE_UNREACHABLE;
}

static inline bool zend_mir_opcode_is_executable_value(
	zend_mir_opcode opcode)
{
	return (opcode >= ZEND_MIR_OPCODE_VALUE_MAKE_REF
			&& opcode <= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM)
		|| opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
		|| (opcode >= ZEND_MIR_OPCODE_VALUE_FE_FREE
			&& opcode <= ZEND_MIR_OPCODE_VALUE_FETCH_LIST)
		|| opcode == ZEND_MIR_OPCODE_VALUE_INCDEC
		|| (opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
			&& opcode <= ZEND_MIR_OPCODE_OBJECT_BIND_STATIC)
		|| opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
		|| opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
		|| (opcode >= ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME
			&& opcode <= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED)
		|| (opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			&& opcode <= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)
		|| opcode == ZEND_MIR_OPCODE_ECHO_SCALAR
		|| opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE;
}

ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_COUNT < UINT32_MAX,
	"opcode invalid value remains unique");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_CALL_DIRECT_USER == ZEND_MIR_OPCODE_COUNT,
	"W05 call opcode begins after the frozen W03 scalar range");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W05_OPCODE_COUNT == ZEND_MIR_OPCODE_CALL_DIRECT_USER + 1,
	"W05 call opcode has an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_STORAGE_BIND == ZEND_MIR_W05_OPCODE_COUNT,
	"W06 value opcodes begin after the frozen W05 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W06_OPCODE_COUNT == ZEND_MIR_OPCODE_SEPARATION_PLAN + 1,
	"W06 value opcodes have an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL == ZEND_MIR_W06_OPCODE_COUNT,
	"W08 internal-call opcode begins after the frozen W06 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_CATCH_ENTER == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL + 1,
	"W08 catch entry follows the internal-call opcode");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_FINALLY_ENTER == ZEND_MIR_OPCODE_CATCH_ENTER + 1,
	"W08 finally entry follows catch entry");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_FINALLY_CALL == ZEND_MIR_OPCODE_FINALLY_ENTER + 1,
	"W08 finally call follows finally entry");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_FINALLY_RETURN == ZEND_MIR_OPCODE_FINALLY_CALL + 1,
	"W08 finally return follows finally call");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
	== ZEND_MIR_OPCODE_FINALLY_RETURN + 1,
	"W08 source-zval return follows finally return");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W08_OPCODE_COUNT
	== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL + 1,
	"W08 opcodes have an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_VALUE_MAKE_REF
	== ZEND_MIR_W08_OPCODE_COUNT,
	"executable value opcodes begin after the W08 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W09_OPCODE_COUNT
	== ZEND_MIR_OPCODE_VALUE_INCDEC + 1,
	"executable value opcodes have an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_VALUE_COND_BRANCH
	== ZEND_MIR_OPCODE_VALUE_FETCH_LIST + 1,
	"source value branch follows executable value operations");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_VALUE_INCDEC
	== ZEND_MIR_OPCODE_VALUE_COND_BRANCH + 1,
	"increment and decrement extend the W09 value range");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
	== ZEND_MIR_W09_OPCODE_COUNT,
	"object operations begin after the W09 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W10_OPCODE_COUNT
	== ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED + 1,
	"W10 object operations have an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
	== ZEND_MIR_W10_OPCODE_COUNT,
	"W11 dynamic operations begin after the W10 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W11_OPCODE_COUNT
	== ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL + 1,
	"W11 dynamic operations have an additive table boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_OPCODE_ECHO_SCALAR
	== ZEND_MIR_W11_OPCODE_COUNT,
	"W11P semantic echo begins after the frozen W11 boundary");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_W11P_OPCODE_COUNT
	== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE + 1,
	"W11P semantic operations have an additive table boundary");

#endif /* ZEND_MIR_OPCODES_H */
