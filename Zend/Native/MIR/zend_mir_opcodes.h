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

#define ZEND_MIR_OPCODE_ENUM(symbol, label, value) ZEND_MIR_OPCODE_##symbol = value,
typedef enum _zend_mir_opcode {
	ZEND_MIR_OPCODE_CATALOG(ZEND_MIR_OPCODE_ENUM)
	ZEND_MIR_OPCODE_COUNT = 10,
	ZEND_MIR_OPCODE_INVALID = UINT32_MAX
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
	ZEND_MIR_REPRESENTATION_INVALID = UINT32_MAX
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
	ZEND_MIR_CONSTANT_KIND_INVALID = UINT32_MAX
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

#endif /* ZEND_MIR_OPCODES_H */
