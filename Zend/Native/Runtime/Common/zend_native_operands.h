/* Decoding of compiler-encoded source operands passed to runtime helpers. */

#ifndef ZEND_NATIVE_OPERANDS_H
#define ZEND_NATIVE_OPERANDS_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Lowering/zend_mir_lowering_source.h"

/*
 * Every helper call decodes its operands, so this sits on the hot path of
 * all helper-backed operations. The encoding is produced by the native
 * compiler from the executing op_array, not from user input: validate it
 * only in debug builds.
 */
static zend_always_inline bool zend_native_decode_explicit_operand(
	const zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	const zend_op_array *op_array = &execute_data->func->op_array;
	const uint32_t index = (uint32_t) (encoded >> 16);

	ZEND_ASSERT(ZEND_USER_CODE(execute_data->func->type));
	switch ((zend_mir_source_operand_kind) (encoded & UINT64_C(0xff))) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			*operand_type = IS_UNUSED;
			operand->num = index == ZEND_MIR_ID_INVALID ? 0 : index;
			return true;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			ZEND_ASSERT(index < (uint32_t) op_array->last_literal);
			*operand_type = IS_CONST;
			operand->constant = index;
			return true;
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			break;
		default:
			return false;
	}
	switch ((zend_mir_source_slot_kind) ((encoded >> 8) & UINT64_C(0xff))) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			ZEND_ASSERT(index < (uint32_t) op_array->last_var);
			*operand_type = IS_CV;
			operand->var = EX_NUM_TO_VAR(index);
			return true;
		case ZEND_MIR_SOURCE_SLOT_TMP:
			ZEND_ASSERT(index < op_array->T);
			*operand_type = IS_TMP_VAR;
			break;
		case ZEND_MIR_SOURCE_SLOT_VAR:
			ZEND_ASSERT(index < op_array->T);
			*operand_type = IS_VAR;
			break;
		default:
			return false;
	}
	operand->var = EX_NUM_TO_VAR((uint32_t) op_array->last_var + index);
	return true;
}

#endif /* ZEND_NATIVE_OPERANDS_H */
