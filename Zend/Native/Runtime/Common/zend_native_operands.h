/* Source operands passed to runtime helpers. */

#ifndef ZEND_NATIVE_OPERANDS_H
#define ZEND_NATIVE_OPERANDS_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Lowering/zend_mir_lowering_source.h"

/*
 * A helper takes a source operand as the VM's opline does: the operand type
 * (IS_UNUSED, IS_CONST, IS_TMP_VAR, IS_VAR or IS_CV) in bits 0-7 and the
 * znode_op value in bits 8-39, which is the frame byte offset of a variable
 * (ZEND_CALL_VAR), the literal index of a constant or the payload of an
 * unused operand. The compiler encodes it from the executing op_array.
 */
#define ZEND_NATIVE_OPERAND_INVALID 0xffu
#define ZEND_NATIVE_OPERAND_TYPE(encoded) ((uint8_t) ((encoded) & 0xff))
#define ZEND_NATIVE_OPERAND_VALUE(encoded) ((uint32_t) ((encoded) >> 8))

/* The variable of a CV, TMP or VAR operand. */
#define ZEND_NATIVE_OPERAND_VAR(execute_data, encoded) \
	ZEND_CALL_VAR((execute_data), ZEND_NATIVE_OPERAND_VALUE(encoded))

#ifndef __cplusplus
static zend_always_inline bool zend_native_decode_explicit_operand(
	const zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	(void) execute_data;
	*operand_type = ZEND_NATIVE_OPERAND_TYPE(encoded);
	operand->num = ZEND_NATIVE_OPERAND_VALUE(encoded);
	return *operand_type != ZEND_NATIVE_OPERAND_INVALID;
}

/* The operand in the compiler's source form, for the runtime paths that
 * work on zend_mir_source_operand_ref. */
static zend_always_inline bool zend_native_decode_source_operand(
	const zend_execute_data *execute_data, uint64_t encoded,
	zend_mir_source_operand_ref *operand)
{
	const uint32_t value = ZEND_NATIVE_OPERAND_VALUE(encoded);

	memset(operand, 0, sizeof(*operand));
	operand->ssa_variable_id = ZEND_MIR_ID_INVALID;
	switch (ZEND_NATIVE_OPERAND_TYPE(encoded)) {
		case IS_UNUSED:
			operand->kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
			operand->index = value == 0 ? ZEND_MIR_ID_INVALID : value;
			return true;
		case IS_CONST:
			operand->kind = ZEND_MIR_SOURCE_OPERAND_LITERAL;
			operand->index = value;
			return true;
		case IS_CV:
			operand->kind = ZEND_MIR_SOURCE_OPERAND_SLOT;
			operand->slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
			operand->index = EX_VAR_TO_NUM(value);
			return true;
		case IS_TMP_VAR:
		case IS_VAR:
			operand->kind = ZEND_MIR_SOURCE_OPERAND_SLOT;
			operand->slot_kind =
				ZEND_NATIVE_OPERAND_TYPE(encoded) == IS_TMP_VAR
					? ZEND_MIR_SOURCE_SLOT_TMP : ZEND_MIR_SOURCE_SLOT_VAR;
			operand->index = EX_VAR_TO_NUM(value)
				- (uint32_t) execute_data->func->op_array.last_var;
			return true;
		default:
			return false;
	}
}

/* The inverse of zend_native_decode_source_operand(). */
static zend_always_inline uint64_t zend_native_encode_source_operand(
	const zend_execute_data *execute_data,
	const zend_mir_source_operand_ref *operand)
{
	uint64_t type = ZEND_NATIVE_OPERAND_INVALID;
	uint64_t value = 0;

	switch (operand->kind) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			type = IS_UNUSED;
			value = operand->index == ZEND_MIR_ID_INVALID ? 0 : operand->index;
			break;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			type = IS_CONST;
			value = operand->index;
			break;
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			if (operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
				type = IS_CV;
				value = EX_NUM_TO_VAR(operand->index);
			} else if (operand->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
					|| operand->slot_kind == ZEND_MIR_SOURCE_SLOT_VAR) {
				type = operand->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
					? IS_TMP_VAR : IS_VAR;
				value = EX_NUM_TO_VAR(
					(uint32_t) execute_data->func->op_array.last_var
						+ operand->index);
			}
			break;
		default:
			break;
	}
	return type | ((value & UINT32_MAX) << 8);
}
#endif

#endif /* ZEND_NATIVE_OPERANDS_H */
