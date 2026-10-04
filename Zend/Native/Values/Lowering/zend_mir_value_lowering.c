/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_ssa.h"

#include "zend_mir_value_lowering.h"

#include "../../Calls/Model/zend_mir_call_model.h"
#include "../../Lowering/Core/zend_mir_lowering_internal.h"
#include "../../Lowering/StraightLine/zend_mir_straight_line_internal.h"
#include "../../Lowering/zend_mir_lowering_zend.h"
#include "../../MIR/Core/zend_mir_module_internal.h"
#include "../../MIR/Semantics/zend_mir_effect_summary.h"

#define ZEND_MIR_SNAPSHOT_MAGIC UINT32_C(0x57365350)
#define ZEND_MIR_LIMIT UINT32_C(1048576)

typedef struct _zend_mir_records {
	uint32_t magic;
	zend_mir_source_storage_ref *source_storages;
	zend_mir_source_reference_ref *source_references;
	zend_mir_source_indirect_ref *source_indirects;
	zend_mir_source_opcode_ref *source_opcodes;
	zend_mir_source_call_site_ref *call_sites;
	zend_mir_source_call_target_ref *call_targets;
	zend_mir_source_call_argument_ref *call_arguments;
	zend_mir_source_parameter_mode_ref *call_parameter_modes;
	zend_mir_value_lowering_inventory_entry *entries;
	zend_mir_storage_ref *storages;
	zend_mir_payload_ref *payloads;
	zend_mir_reference_cell_ref *references;
	zend_mir_alias_relation_ref *aliases;
	zend_mir_ownership_event_ref *events;
	zend_mir_separation_plan_ref *separations;
	zend_mir_call_transfer_ref *transfers;
	zend_mir_storage_id *call_return_storage_ids;
	zend_mir_storage_id *call_argument_storage_ids;
	uint32_t source_storage_count;
	uint32_t source_reference_count;
	uint32_t source_indirect_count;
	uint32_t source_opcode_count;
	uint32_t call_site_count;
	uint32_t call_target_count;
	uint32_t call_argument_count;
	uint32_t call_parameter_mode_count;
	uint32_t entry_count;
	uint32_t storage_count;
	uint32_t payload_count;
	uint32_t reference_count;
	uint32_t alias_count;
	uint32_t event_count;
	uint32_t separation_count;
	uint32_t transfer_count;
	zend_mir_source_call_view call_calls;
	zend_mir_source_call_target_resolver call_resolver;
} zend_mir_records;

static void *zend_mir_value_calloc(uint32_t count, size_t size)
{
	if (count == 0) {
		return NULL;
	}
	if (count > ZEND_MIR_LIMIT
			|| (size != 0 && count > SIZE_MAX / size)) {
		return NULL;
	}
	return calloc(count, size);
}

bool zend_mir_opcode_is_accepted(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_ASSIGN:
		case ZEND_ASSIGN_REF:
		case ZEND_CHECK_VAR:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_REF:
		case ZEND_FREE:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_RETURN_BY_REF:
		case ZEND_MAKE_REF:
		case ZEND_UNSET_CV:
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_SEPARATE:
		case ZEND_COPY_TMP:
		case ZEND_SEND_FUNC_ARG:
			return true;
		default:
			return false;
	}
}

zend_mir_opcode zend_mir_executable_opcode(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_DECLARE_ANON_CLASS:
			return ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS;
		case ZEND_FETCH_THIS:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_THIS;
		case ZEND_FETCH_OBJ_R:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_R;
		case ZEND_FETCH_OBJ_W:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_W;
		case ZEND_FETCH_OBJ_RW:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_RW;
		case ZEND_FETCH_OBJ_IS:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_IS;
		case ZEND_FETCH_OBJ_FUNC_ARG:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG;
		case ZEND_FETCH_OBJ_UNSET:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_UNSET;
		case ZEND_ASSIGN_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_ASSIGN;
		case ZEND_ASSIGN_OBJ_REF:
			return ZEND_MIR_OPCODE_OBJECT_ASSIGN_REF;
		case ZEND_ASSIGN_OBJ_OP:
			return ZEND_MIR_OPCODE_OBJECT_ASSIGN_OP;
		case ZEND_UNSET_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_UNSET;
		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_ISSET_ISEMPTY;
		case ZEND_PRE_INC_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_PRE_INC;
		case ZEND_PRE_DEC_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_PRE_DEC;
		case ZEND_POST_INC_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_POST_INC;
		case ZEND_POST_DEC_OBJ:
			return ZEND_MIR_OPCODE_OBJECT_POST_DEC;
		case ZEND_INSTANCEOF:
			return ZEND_MIR_OPCODE_OBJECT_INSTANCEOF;
		case ZEND_CLONE:
			return ZEND_MIR_OPCODE_OBJECT_CLONE;
		case ZEND_FETCH_STATIC_PROP_R:
			return ZEND_MIR_OPCODE_STATIC_FETCH_R;
		case ZEND_FETCH_STATIC_PROP_W:
			return ZEND_MIR_OPCODE_STATIC_FETCH_W;
		case ZEND_FETCH_STATIC_PROP_RW:
			return ZEND_MIR_OPCODE_STATIC_FETCH_RW;
		case ZEND_FETCH_STATIC_PROP_IS:
			return ZEND_MIR_OPCODE_STATIC_FETCH_IS;
		case ZEND_FETCH_STATIC_PROP_FUNC_ARG:
			return ZEND_MIR_OPCODE_STATIC_FETCH_FUNC_ARG;
		case ZEND_FETCH_STATIC_PROP_UNSET:
			return ZEND_MIR_OPCODE_STATIC_FETCH_UNSET;
		case ZEND_ASSIGN_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_ASSIGN;
		case ZEND_ASSIGN_STATIC_PROP_REF:
			return ZEND_MIR_OPCODE_STATIC_ASSIGN_REF;
		case ZEND_ASSIGN_STATIC_PROP_OP:
			return ZEND_MIR_OPCODE_STATIC_ASSIGN_OP;
		case ZEND_PRE_INC_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_PRE_INC;
		case ZEND_PRE_DEC_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_PRE_DEC;
		case ZEND_POST_INC_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_POST_INC;
		case ZEND_POST_DEC_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_POST_DEC;
		case ZEND_ISSET_ISEMPTY_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_ISSET_ISEMPTY;
		case ZEND_UNSET_STATIC_PROP:
			return ZEND_MIR_OPCODE_STATIC_UNSET;
		case ZEND_FETCH_CLASS:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS;
		case ZEND_GET_CLASS:
		case ZEND_FETCH_CLASS_NAME:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME;
		case ZEND_FETCH_CLASS_CONSTANT:
			return ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_CONSTANT;
		case ZEND_DECLARE_LAMBDA_FUNCTION:
			return ZEND_MIR_OPCODE_OBJECT_DECLARE_LAMBDA;
		case ZEND_BIND_LEXICAL:
			return ZEND_MIR_OPCODE_OBJECT_BIND_LEXICAL;
		case ZEND_BIND_STATIC:
			return ZEND_MIR_OPCODE_OBJECT_BIND_STATIC;
		case ZEND_DECLARE_FUNCTION:
			return ZEND_MIR_OPCODE_OBJECT_DECLARE_FUNCTION;
		case ZEND_DECLARE_CLASS:
			return ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS;
		case ZEND_DECLARE_CLASS_DELAYED:
			return ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED;
		case ZEND_FETCH_R:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_R;
		case ZEND_FETCH_W:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_W;
		case ZEND_FETCH_RW:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_RW;
		case ZEND_FETCH_IS:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_IS;
		case ZEND_FETCH_FUNC_ARG:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_FUNC_ARG;
		case ZEND_FETCH_UNSET:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_UNSET;
		case ZEND_UNSET_VAR:
			return ZEND_MIR_OPCODE_DYNAMIC_UNSET_VAR;
		case ZEND_ISSET_ISEMPTY_VAR:
			return ZEND_MIR_OPCODE_DYNAMIC_ISSET_ISEMPTY_VAR;
		case ZEND_BIND_GLOBAL:
			return ZEND_MIR_OPCODE_DYNAMIC_BIND_GLOBAL;
		case ZEND_FETCH_GLOBALS:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_GLOBALS;
		case ZEND_FETCH_CONSTANT:
			return ZEND_MIR_OPCODE_DYNAMIC_FETCH_CONSTANT;
		case ZEND_DECLARE_CONST:
			return ZEND_MIR_OPCODE_DYNAMIC_DECLARE_CONSTANT;
		case ZEND_DECLARE_ATTRIBUTED_CONST:
			return ZEND_MIR_OPCODE_DYNAMIC_DECLARE_ATTRIBUTED_CONSTANT;
		case ZEND_INCLUDE_OR_EVAL:
			return ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL;
		case ZEND_TYPE_CHECK:
			return ZEND_MIR_OPCODE_VALUE_TYPE_CHECK;
		case ZEND_FRAMELESS_ICALL_0:
		case ZEND_FRAMELESS_ICALL_1:
		case ZEND_FRAMELESS_ICALL_2:
		case ZEND_FRAMELESS_ICALL_3:
			return ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL;
		case ZEND_ECHO:
			return ZEND_MIR_OPCODE_VALUE_ECHO;
		case ZEND_VERIFY_RETURN_TYPE:
			return ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE;
		case ZEND_FUNC_NUM_ARGS:
			return ZEND_MIR_OPCODE_FUNC_NUM_ARGS;
		case ZEND_FUNC_GET_ARGS:
			return ZEND_MIR_OPCODE_FUNC_GET_ARGS;
		case ZEND_GENERATOR_CREATE:
			return ZEND_MIR_OPCODE_GENERATOR_CREATE;
		case ZEND_YIELD:
			return ZEND_MIR_OPCODE_GENERATOR_YIELD;
		case ZEND_YIELD_FROM:
			return ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM;
		case ZEND_GENERATOR_RETURN:
			return ZEND_MIR_OPCODE_GENERATOR_RETURN;
		case ZEND_SWITCH_LONG:
		case ZEND_SWITCH_STRING:
		case ZEND_MATCH:
			return ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH;
		case ZEND_COUNT:
			return ZEND_MIR_OPCODE_VALUE_COUNT;
		case ZEND_GET_TYPE:
			return ZEND_MIR_OPCODE_VALUE_GET_TYPE;
		case ZEND_ARRAY_KEY_EXISTS:
			return ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS;
		case ZEND_IN_ARRAY:
			return ZEND_MIR_OPCODE_VALUE_IN_ARRAY;
		case ZEND_ISSET_ISEMPTY_THIS:
			return ZEND_MIR_OPCODE_VALUE_ISSET_THIS;
		case ZEND_GET_CALLED_CLASS:
			return ZEND_MIR_OPCODE_VALUE_GET_CALLED_CLASS;
		case ZEND_BEGIN_SILENCE:
			return ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE;
		case ZEND_END_SILENCE:
			return ZEND_MIR_OPCODE_VALUE_END_SILENCE;
		case ZEND_MATCH_ERROR:
			return ZEND_MIR_OPCODE_VALUE_MATCH_ERROR;
		case ZEND_VERIFY_NEVER_TYPE:
			return ZEND_MIR_OPCODE_VALUE_VERIFY_NEVER_TYPE;
		case ZEND_DEFINED:
			return ZEND_MIR_OPCODE_VALUE_DEFINED;
		case ZEND_TICKS:
			return ZEND_MIR_OPCODE_VALUE_TICKS;
		case ZEND_TYPE_ASSERT:
			return ZEND_MIR_OPCODE_VALUE_TYPE_ASSERT;
		case ZEND_EXT_STMT:
			return ZEND_MIR_OPCODE_VALUE_EXT_STMT;
		case ZEND_EXT_FCALL_BEGIN:
			return ZEND_MIR_OPCODE_VALUE_EXT_FCALL_BEGIN;
		case ZEND_EXT_FCALL_END:
			return ZEND_MIR_OPCODE_VALUE_EXT_FCALL_END;
		case ZEND_EXT_NOP:
			return ZEND_MIR_OPCODE_VALUE_EXT_NOP;
		case ZEND_DISCARD_EXCEPTION:
			return ZEND_MIR_OPCODE_VALUE_DISCARD_EXCEPTION;
		case ZEND_CASE:
		case ZEND_CASE_STRICT:
			return ZEND_MIR_OPCODE_VALUE_CASE;
		case ZEND_BIND_INIT_STATIC_OR_JMP:
			return ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH;
		case ZEND_JMP_FRAMELESS:
			return ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH;
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_DIV:
		case ZEND_MOD:
		case ZEND_POW:
		case ZEND_SL:
		case ZEND_SR:
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
		case ZEND_BOOL_XOR:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_SPACESHIP:
			return ZEND_MIR_OPCODE_VALUE_BINARY_OP;
		case ZEND_BW_NOT:
		case ZEND_BOOL_NOT:
		case ZEND_BOOL:
		case ZEND_STRLEN:
			return ZEND_MIR_OPCODE_VALUE_UNARY_OP;
		case ZEND_CAST:
			return ZEND_MIR_OPCODE_VALUE_CAST;
		case ZEND_ISSET_ISEMPTY_CV:
			return ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV;
		case ZEND_FETCH_LIST_R:
		case ZEND_FETCH_LIST_W:
			return ZEND_MIR_OPCODE_VALUE_FETCH_LIST;
		case ZEND_PRE_INC:
		case ZEND_PRE_DEC:
		case ZEND_POST_INC:
		case ZEND_POST_DEC:
			return ZEND_MIR_OPCODE_VALUE_INCDEC;
		case ZEND_ASSIGN:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN;
		case ZEND_ASSIGN_OP:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_OP;
		case ZEND_QM_ASSIGN:
			return ZEND_MIR_OPCODE_VALUE_QM_ASSIGN;
		case ZEND_CONCAT:
			return ZEND_MIR_OPCODE_VALUE_CONCAT;
		case ZEND_FAST_CONCAT:
			return ZEND_MIR_OPCODE_VALUE_FAST_CONCAT;
		case ZEND_ROPE_INIT:
			return ZEND_MIR_OPCODE_VALUE_ROPE_INIT;
		case ZEND_ROPE_ADD:
			return ZEND_MIR_OPCODE_VALUE_ROPE_ADD;
		case ZEND_ROPE_END:
			return ZEND_MIR_OPCODE_VALUE_ROPE_END;
		case ZEND_INIT_ARRAY:
			return ZEND_MIR_OPCODE_VALUE_INIT_ARRAY;
		case ZEND_ADD_ARRAY_ELEMENT:
			return ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT;
		case ZEND_ADD_ARRAY_UNPACK:
			return ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK;
		case ZEND_FETCH_DIM_R:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R;
		case ZEND_FETCH_DIM_W:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W;
		case ZEND_FETCH_DIM_RW:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW;
		case ZEND_FETCH_DIM_IS:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS;
		case ZEND_FETCH_DIM_FUNC_ARG:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG;
		case ZEND_FETCH_DIM_UNSET:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET;
		case ZEND_ASSIGN_DIM:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM;
		case ZEND_ASSIGN_DIM_OP:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP;
		case ZEND_UNSET_DIM:
			return ZEND_MIR_OPCODE_VALUE_UNSET_DIM;
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
			return ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM;
		case ZEND_FE_FREE:
			return ZEND_MIR_OPCODE_VALUE_FE_FREE;
		case ZEND_MAKE_REF:
			return ZEND_MIR_OPCODE_VALUE_MAKE_REF;
		case ZEND_ASSIGN_REF:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_REF;
		case ZEND_SEPARATE:
			return ZEND_MIR_OPCODE_VALUE_SEPARATE;
		case ZEND_COPY_TMP:
			return ZEND_MIR_OPCODE_VALUE_COPY_TMP;
		case ZEND_FREE:
			return ZEND_MIR_OPCODE_VALUE_FREE;
		case ZEND_UNSET_CV:
			return ZEND_MIR_OPCODE_VALUE_UNSET_CV;
		case ZEND_CHECK_VAR:
			return ZEND_MIR_OPCODE_VALUE_CHECK_VAR;
		case ZEND_CHECK_FUNC_ARG:
			return ZEND_MIR_OPCODE_VALUE_CHECK_FUNC_ARG;
		case ZEND_CHECK_UNDEF_ARGS:
			return ZEND_MIR_OPCODE_VALUE_CHECK_UNDEF_ARGS;
		default:
			return ZEND_MIR_OPCODE_INVALID;
	}
}

static zend_mir_opcode zend_mir_control_value_opcode(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
		case ZEND_JMP_SET:
		case ZEND_COALESCE:
		case ZEND_JMP_NULL:
		case ZEND_ASSERT_CHECK:
			return ZEND_MIR_OPCODE_VALUE_COND_BRANCH;
		case ZEND_FE_RESET_R:
		case ZEND_FE_RESET_RW:
		case ZEND_FE_FETCH_R:
		case ZEND_FE_FETCH_RW:
			return ZEND_MIR_OPCODE_ITERATOR_BRANCH;
		case ZEND_THROW:
			return ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL;
		case ZEND_RETURN:
		case ZEND_RETURN_BY_REF:
			return ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL;
		default:
			return ZEND_MIR_OPCODE_INVALID;
	}
}

bool zend_mir_object_opcode_is_executable(uint32_t opcode)
{
	return opcode == ZEND_OP_DATA
		|| opcode == ZEND_FE_RESET_R || opcode == ZEND_FE_FETCH_R
		|| opcode == ZEND_FE_RESET_RW || opcode == ZEND_FE_FETCH_RW
		|| zend_mir_executable_opcode(opcode) != ZEND_MIR_OPCODE_INVALID;
}

bool zend_mir_overlay_opcode_is_executable(uint32_t opcode)
{
	zend_mir_opcode mapped = zend_mir_executable_opcode(opcode);

	return zend_mir_object_opcode_is_executable(opcode)
		|| (mapped >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			&& mapped <= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL);
}

static bool zend_mir_value_add_effect(
	zend_mir_effect_summary *summary, zend_mir_effect effect)
{
	zend_mir_effect_summary atomic;
	zend_mir_effect_summary composed;

	if (!zend_mir_effect_summary_from_effect(effect, &atomic)
			|| !zend_mir_effect_summary_compose(
				&composed, summary, &atomic)) {
		return false;
	}
	*summary = composed;
	return true;
}

static bool zend_mir_operation_semantics(
	zend_mir_opcode opcode, zend_mir_executable_value_ref *operation,
	zend_mir_safepoint_class *frame_class)
{
	zend_mir_effect_summary summary;
	const zend_mir_memory_domain_mask frame_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_LOCALS)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_TEMPS);
	const zend_mir_memory_domain_mask argument_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_ARGS);
	const zend_mir_memory_domain_mask array_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ARRAY);
	const zend_mir_memory_domain_mask reference_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_REFERENCE)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_GC_METADATA);
	const zend_mir_memory_domain_mask generator_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_GENERATOR)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_OBJECT);
	zend_mir_effect_summary_empty(&summary);
	if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_READ_MEMORY)
			|| !zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_WRITE_MEMORY)) {
		return false;
	}
	switch (opcode) {
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_ALLOCATE)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
		case ZEND_MIR_OPCODE_VALUE_ROPE_END:
		case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
		case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
		case ZEND_MIR_OPCODE_VALUE_FE_FREE:
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
		case ZEND_MIR_OPCODE_VALUE_CAST:
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
		case ZEND_MIR_OPCODE_VALUE_CASE:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_THIS:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_R:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_W:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_RW:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_IS:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_UNSET:
		case ZEND_MIR_OPCODE_OBJECT_ASSIGN:
		case ZEND_MIR_OPCODE_OBJECT_ASSIGN_REF:
		case ZEND_MIR_OPCODE_OBJECT_ASSIGN_OP:
		case ZEND_MIR_OPCODE_OBJECT_UNSET:
		case ZEND_MIR_OPCODE_OBJECT_ISSET_ISEMPTY:
		case ZEND_MIR_OPCODE_OBJECT_PRE_INC:
		case ZEND_MIR_OPCODE_OBJECT_PRE_DEC:
		case ZEND_MIR_OPCODE_OBJECT_POST_INC:
		case ZEND_MIR_OPCODE_OBJECT_POST_DEC:
		case ZEND_MIR_OPCODE_OBJECT_INSTANCEOF:
		case ZEND_MIR_OPCODE_OBJECT_CLONE:
		case ZEND_MIR_OPCODE_STATIC_FETCH_R:
		case ZEND_MIR_OPCODE_STATIC_FETCH_W:
		case ZEND_MIR_OPCODE_STATIC_FETCH_RW:
		case ZEND_MIR_OPCODE_STATIC_FETCH_IS:
		case ZEND_MIR_OPCODE_STATIC_FETCH_FUNC_ARG:
		case ZEND_MIR_OPCODE_STATIC_FETCH_UNSET:
		case ZEND_MIR_OPCODE_STATIC_ASSIGN:
		case ZEND_MIR_OPCODE_STATIC_ASSIGN_REF:
		case ZEND_MIR_OPCODE_STATIC_ASSIGN_OP:
		case ZEND_MIR_OPCODE_STATIC_PRE_INC:
		case ZEND_MIR_OPCODE_STATIC_PRE_DEC:
		case ZEND_MIR_OPCODE_STATIC_POST_INC:
		case ZEND_MIR_OPCODE_STATIC_POST_DEC:
		case ZEND_MIR_OPCODE_STATIC_ISSET_ISEMPTY:
		case ZEND_MIR_OPCODE_STATIC_UNSET:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME:
		case ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_CONSTANT:
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_LAMBDA:
		case ZEND_MIR_OPCODE_OBJECT_BIND_LEXICAL:
		case ZEND_MIR_OPCODE_OBJECT_BIND_STATIC:
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_FUNCTION:
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS:
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_R:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_W:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_RW:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_IS:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_FUNC_ARG:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_UNSET:
		case ZEND_MIR_OPCODE_DYNAMIC_UNSET_VAR:
		case ZEND_MIR_OPCODE_DYNAMIC_ISSET_ISEMPTY_VAR:
		case ZEND_MIR_OPCODE_DYNAMIC_BIND_GLOBAL:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_GLOBALS:
		case ZEND_MIR_OPCODE_DYNAMIC_FETCH_CONSTANT:
		case ZEND_MIR_OPCODE_DYNAMIC_DECLARE_CONSTANT:
		case ZEND_MIR_OPCODE_DYNAMIC_DECLARE_ATTRIBUTED_CONSTANT:
		case ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL:
		case ZEND_MIR_OPCODE_ECHO_SCALAR:
		case ZEND_MIR_OPCODE_VALUE_ECHO:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_CALL_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_TYPE_CHECK:
		case ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_FUNC_NUM_ARGS:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_FUNC_GET_ARGS:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_CALL_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_GENERATOR_CREATE:
		case ZEND_MIR_OPCODE_GENERATOR_YIELD:
		case ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_SUSPEND)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_GENERATOR_RETURN:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_COUNT:
		case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
		case ZEND_MIR_OPCODE_VALUE_IN_ARRAY:
		case ZEND_MIR_OPCODE_VALUE_GET_TYPE:
		case ZEND_MIR_OPCODE_VALUE_GET_CALLED_CLASS:
		case ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE:
		case ZEND_MIR_OPCODE_VALUE_END_SILENCE:
		case ZEND_MIR_OPCODE_VALUE_MATCH_ERROR:
		case ZEND_MIR_OPCODE_VALUE_VERIFY_NEVER_TYPE:
		case ZEND_MIR_OPCODE_VALUE_DEFINED:
		case ZEND_MIR_OPCODE_VALUE_TICKS:
		case ZEND_MIR_OPCODE_VALUE_TYPE_ASSERT:
		case ZEND_MIR_OPCODE_VALUE_EXT_STMT:
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_BEGIN:
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_END:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_EXT_NOP:
			break;
		case ZEND_MIR_OPCODE_VALUE_ISSET_THIS:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL:
			if (!zend_mir_value_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_FREE:
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
		case ZEND_MIR_OPCODE_VALUE_DISCARD_EXCEPTION:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
		case ZEND_MIR_OPCODE_VALUE_CHECK_FUNC_ARG:
		case ZEND_MIR_OPCODE_VALUE_CHECK_UNDEF_ARGS:
			if (!zend_mir_value_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_value_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
		case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
			break;
		default:
			return false;
	}
	if (opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS) {
		summary.reads |= argument_domains;
		summary.writes |= frame_domains;
	} else if (opcode == ZEND_MIR_OPCODE_FUNC_GET_ARGS) {
		summary.reads |= argument_domains | reference_domains;
		summary.writes |= frame_domains | array_domains | reference_domains;
	} else {
		summary.reads |= frame_domains | reference_domains;
		summary.writes |= frame_domains | reference_domains;
	}
	if (opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
			|| opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
			|| opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM
			|| opcode == ZEND_MIR_OPCODE_GENERATOR_RETURN) {
		summary.reads |= generator_domains;
		summary.writes |= generator_domains;
	}
	if (!zend_mir_effect_summary_init(&summary, summary.effects,
			summary.reads, summary.writes, summary.barriers,
			opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS
				? ZEND_MIR_OWNERSHIP_ACTION_MASK(
					ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED)
				: 0,
			0)) {
		return false;
	}
	operation->effects = summary.effects;
	operation->reads = summary.reads;
	operation->writes = summary.writes;
	operation->barriers = summary.barriers;
	operation->ownership_actions = summary.ownership_actions;
	if (opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
			|| opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	} else if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR;
	} else if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_EXCEPTION_EDGE;
	} else if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	} else if ((summary.effects
			& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_ALLOCATE)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_ALLOCATION;
	} else {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_INVALID;
	}
	return true;
}

static bool zend_mir_value_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out)
{
	return source != NULL && out != NULL
		&& id < source->block_count(source->context)
		&& source->block_at(source->context, id, out)
		&& out->id == id;
}

static bool zend_mir_build_block_index(
	const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map, const zend_mir_view *view,
	zend_mir_block_id *mir_block_by_source, uint32_t source_block_count)
{
	uint32_t index;
	uint32_t reachable_index = 0;

	if (source == NULL || view == NULL || mir_block_by_source == NULL
			|| source->block_count == NULL || source->block_at == NULL
			|| source->block_count(source->context) != source_block_count
			|| view->block_count == NULL || view->block_at == NULL) {
		return false;
	}
	for (index = 0; index < source_block_count; index++) {
		mir_block_by_source[index] = ZEND_MIR_ID_INVALID;
	}
	if (map != NULL && map->block_count != NULL && map->block_at != NULL) {
		for (index = 0; index < map->block_count(map->context); index++) {
			zend_mir_control_flow_block_mapping mapping;
			if (!map->block_at(map->context, index, &mapping)
					|| mapping.source_block_id >= source_block_count
					|| zend_mir_id_is_valid(
						mir_block_by_source[mapping.source_block_id])) {
				return false;
			}
			mir_block_by_source[mapping.source_block_id] =
				mapping.mir_block_id;
		}
	}
	/*
	 * Control-flow lowering invalidates its process-local block map once it
	 * returns. Its persistent block order is nevertheless source-backed: reachable
	 * source blocks are created once, in source-table order, before calls or
	 * value operations are appended. Reconstruct only that stable ordinal.
	 */
	for (index = 0; index < source->block_count(source->context); index++) {
		zend_mir_source_block_ref source_block;
		zend_mir_block_record mir_block;

		if (!source->block_at(source->context, index, &source_block)) {
			return false;
		}
		if ((source_block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if (!zend_mir_id_is_valid(
				mir_block_by_source[source_block.id])) {
			if (reachable_index >= view->block_count(view->context)
					|| !view->block_at(
						view->context, reachable_index, &mir_block)) {
				return false;
			}
			mir_block_by_source[source_block.id] = mir_block.id;
		}
		reachable_index++;
	}
	return true;
}

static int zend_mir_compare_operations(const void *left, const void *right)
{
	const zend_mir_executable_value_ref *a = left;
	const zend_mir_executable_value_ref *b = right;

	if (a->block_id != b->block_id) {
		return a->block_id < b->block_id ? -1 : 1;
	}
	if (a->source_position_id != b->source_position_id) {
		return a->source_position_id < b->source_position_id ? -1 : 1;
	}
	return 0;
}

static zend_mir_storage_id zend_mir_operand_storage_id(
	const zend_op_array *op_array, const zend_mir_source_operand_ref *operand)
{
	uint32_t base;

	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return ZEND_MIR_ID_INVALID;
	}
	if (operand->slot_kind < ZEND_MIR_SOURCE_SLOT_CV
			|| operand->slot_kind > ZEND_MIR_SOURCE_SLOT_VAR) {
		return ZEND_MIR_ID_INVALID;
	}
	base = operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV
		? 0 : (uint32_t) op_array->last_var;
	if (operand->index > ZEND_MIR_ID_MAX - base
			|| base + operand->index
				>= (uint32_t) op_array->last_var + op_array->T) {
		return ZEND_MIR_ID_INVALID;
	}
	return base + operand->index;
}

static zend_mir_storage_id zend_mir_ssa_storage_id(
	const zend_op_array *op_array, const zend_mir_source_ssa_ref *ssa)
{
	zend_mir_source_operand_ref operand;

	memset(&operand, 0, sizeof(operand));
	operand.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	operand.slot_kind = ssa->source_slot_kind;
	operand.index = ssa->source_slot;
	operand.ssa_variable_id = ssa->ssa_variable_id;
	return zend_mir_operand_storage_id(op_array, &operand);
}

static int zend_mir_compare_value_locations(
	const void *left, const void *right)
{
	const zend_mir_value_location_ref *a = left;
	const zend_mir_value_location_ref *b = right;

	if (a->value_id == b->value_id) {
		return 0;
	}
	return a->value_id < b->value_id ? -1 : 1;
}

static bool zend_mir_index_frame_arguments(
	const zend_op_array *op_array,
	const zend_mir_source_call_view *source,
	uint32_t source_ssa_count,
	uint32_t *argument_by_ssa)
{
	uint32_t index;

	if (op_array == NULL || source == NULL || argument_by_ssa == NULL
			|| source->source_opcode_count == NULL
			|| source->source_opcode_at == NULL
			|| source->source_opcode_count(source->context)
				!= op_array->last) {
		return false;
	}
	for (index = 0; index < source_ssa_count; index++) {
		argument_by_ssa[index] = 0;
	}
	for (index = 0; index < op_array->last; index++) {
		zend_mir_source_opcode_ref opcode;
		uint32_t argument_number;
		uint32_t ssa_variable_id;

		if (!source->source_opcode_at(source->context, index, &opcode)
				|| opcode.opline_index != index) {
			return false;
		}
		if (opcode.zend_opcode_number != ZEND_RECV
				&& opcode.zend_opcode_number != ZEND_RECV_INIT) {
			continue;
		}
		argument_number = op_array->opcodes[index].op1.num;
		ssa_variable_id = opcode.result.ssa_variable_id;
		if (argument_number == 0 || argument_number > op_array->num_args
				|| !zend_mir_id_is_valid(ssa_variable_id)
				|| ssa_variable_id >= source_ssa_count
				|| argument_by_ssa[ssa_variable_id] != 0) {
			return false;
		}
		argument_by_ssa[ssa_variable_id] = argument_number;
	}
	return true;
}

static bool zend_mir_index_control_value_instructions(
	const zend_mir_view *view, uint32_t source_count,
	zend_mir_instruction_id *instructions_by_source)
{
	uint32_t index;

	if (view == NULL || instructions_by_source == NULL
			|| view->instruction_count == NULL
			|| view->instruction_at == NULL) {
		return false;
	}
	for (index = 0; index < source_count; index++) {
		instructions_by_source[index] = ZEND_MIR_ID_INVALID;
	}
	for (index = 0; index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record instruction;

		if (!view->instruction_at(view->context, index, &instruction)) {
			return false;
		}
		if (instruction.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| instruction.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| instruction.opcode
					== ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
				|| instruction.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH
				|| instruction.opcode
					== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| instruction.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
				|| instruction.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				|| instruction.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			if (instruction.source_position_id >= source_count
					|| zend_mir_id_is_valid(instructions_by_source[
						instruction.source_position_id])) {
				return false;
			}
			instructions_by_source[instruction.source_position_id] =
				instruction.id;
		}
	}
	return true;
}

static bool zend_mir_ssa_id_valid(int id, uint32_t value_count)
{
	return id < 0 || (uint32_t) id < value_count;
}

static zend_mir_value_category zend_mir_type_category(uint32_t type);

static zend_mir_refcount_state zend_mir_type_refcount_state(
	uint32_t type, zend_mir_value_category category)
{
	if (category == ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR) {
		return ZEND_MIR_REFCOUNT_IMMORTAL;
	}
	if ((type & MAY_BE_RCN) != 0) {
		return ZEND_MIR_REFCOUNT_SHARED;
	}
	if ((type & MAY_BE_RC1) != 0) {
		return ZEND_MIR_REFCOUNT_UNIQUE;
	}
	return ZEND_MIR_REFCOUNT_UNKNOWN;
}

static void zend_mir_bit_set(uint64_t *words, uint32_t value)
{
	words[value / 64] |= UINT64_C(1) << (value % 64);
}

static void zend_mir_bit_reset(uint64_t *words, uint32_t value)
{
	words[value / 64] &= ~(UINT64_C(1) << (value % 64));
}

static int zend_mir_compare_u32(const void *left, const void *right)
{
	const uint32_t a = *(const uint32_t *) left;
	const uint32_t b = *(const uint32_t *) right;
	return a < b ? -1 : a > b;
}

/*
 * Zend deliberately keeps the synthetic value/JMP tail following a THROW
 * expression in the same SSA block.  It is useful to Zend's live-range
 * analysis, but THROW is still the owning native terminator, so none of the
 * tail opcodes may be staged as executable value operations.
 */
static bool zend_mir_mark_expression_throw_tails(
	const zend_op_array *op_array, const zend_ssa *ssa, uint8_t *tails)
{
	uint32_t block_index;

	if (op_array == NULL || ssa == NULL || tails == NULL
			|| ssa->cfg.blocks == NULL || ssa->cfg.blocks_count == 0) {
		return false;
	}
	for (block_index = 0;
			block_index < ssa->cfg.blocks_count; block_index++) {
		const zend_basic_block *block = &ssa->cfg.blocks[block_index];
		uint32_t index;
		uint32_t end;
		bool terminated = false;

		if ((block->flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		if (block->start > op_array->last
				|| block->len > op_array->last - block->start) {
			return false;
		}
		end = block->start + block->len;
		for (index = block->start; index < end; index++) {
			if (terminated) {
				tails[index] = 1;
				continue;
			}
			if (op_array->opcodes[index].opcode == ZEND_THROW
					&& op_array->opcodes[index].extended_value
						== ZEND_THROW_IS_EXPR) {
				terminated = true;
			}
		}
	}
	return true;
}

static bool zend_mir_freeze_suspend_liveness(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_module *module,
	zend_mir_value_mutator *mutator)
{
	uint64_t *block_use = NULL;
	uint64_t *block_def = NULL;
	uint64_t *block_phi_def = NULL;
	uint64_t *live_in = NULL;
	uint64_t *live_out = NULL;
	uint64_t *next_out = NULL;
	uint64_t *next_in = NULL;
	uint64_t *target_live = NULL;
	uint32_t *targets = NULL;
	uint32_t *worklist = NULL;
	uint8_t *queued = NULL;
	uint32_t target_count = 0;
	uint32_t word_count;
	uint64_t matrix_words;
	uint32_t storage_count;
	uint32_t block_index;
	uint32_t index;
	bool success = false;

	if ((op_array->fn_flags & ZEND_ACC_GENERATOR) == 0) {
		return true;
	}
	if (ssa == NULL || ssa->ops == NULL || ssa->vars == NULL
			|| ssa->blocks == NULL || ssa->cfg.blocks == NULL
			|| ssa->cfg.map == NULL || ssa->vars_count < 0
			|| ssa->cfg.blocks_count > ZEND_MIR_LIMIT
			|| (uint32_t) ssa->vars_count > ZEND_MIR_LIMIT
			|| op_array->last > ZEND_MIR_LIMIT
			|| op_array->last_try_catch
				> ZEND_MIR_LIMIT - op_array->last
			|| op_array->last_var > ZEND_MIR_LIMIT
			|| op_array->T
				> ZEND_MIR_LIMIT - op_array->last_var
			|| mutator == NULL || mutator->add_suspend_live_value == NULL) {
		return false;
	}
	if (ssa->cfg.edges_count != 0 && ssa->cfg.predecessors == NULL) {
		return false;
	}
	storage_count = (uint32_t) op_array->last_var + op_array->T;
	word_count = ((uint32_t) ssa->vars_count + 63) / 64;
	if (word_count == 0) {
		return true;
	}
	matrix_words =
		(uint64_t) ssa->cfg.blocks_count * (uint64_t) word_count;
	if (matrix_words > ZEND_MIR_LIMIT) {
		return false;
	}

	targets = zend_mir_value_calloc(
		op_array->last + op_array->last_try_catch, sizeof(*targets));
	if ((op_array->last != 0 || op_array->last_try_catch != 0)
			&& targets == NULL) {
		goto done;
	}
	if ((op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) != 0) {
		for (index = 0; index < op_array->last_try_catch; index++) {
			const zend_try_catch_element *region =
				&op_array->try_catch_array[index];
			if (region->finally_op != 0
					&& region->finally_op < op_array->last
					&& region->finally_end < op_array->last) {
				targets[target_count++] = region->finally_op;
			}
		}
	}
	for (index = 0; index < op_array->last; index++) {
		const uint8_t opcode = op_array->opcodes[index].opcode;
		if ((opcode == ZEND_GENERATOR_CREATE
					|| opcode == ZEND_YIELD
					|| opcode == ZEND_YIELD_FROM)
				&& index + 1 < op_array->last) {
			const uint32_t source_block = ssa->cfg.map[index];
			if (source_block >= ssa->cfg.blocks_count) {
				goto done;
			}
			if ((ssa->cfg.blocks[source_block].flags
					& ZEND_BB_REACHABLE) != 0) {
				targets[target_count++] = index + 1;
			}
		}
	}
	if (target_count == 0) {
		success = true;
		goto done;
	}
	qsort(targets, target_count, sizeof(*targets), zend_mir_compare_u32);
	{
		uint32_t unique = 1;
		for (index = 1; index < target_count; index++) {
			if (targets[index] != targets[unique - 1]) {
				targets[unique++] = targets[index];
			}
		}
		target_count = unique;
	}

	if (matrix_words != 0) {
		block_use = zend_mir_value_calloc(
			(uint32_t) matrix_words, sizeof(*block_use));
		block_def = zend_mir_value_calloc(
			(uint32_t) matrix_words, sizeof(*block_def));
		block_phi_def = zend_mir_value_calloc(
			(uint32_t) matrix_words, sizeof(*block_phi_def));
		live_in = zend_mir_value_calloc(
			(uint32_t) matrix_words, sizeof(*live_in));
		live_out = zend_mir_value_calloc(
			(uint32_t) matrix_words, sizeof(*live_out));
	}
	next_out = zend_mir_value_calloc(word_count, sizeof(*next_out));
	next_in = zend_mir_value_calloc(word_count, sizeof(*next_in));
	target_live = zend_mir_value_calloc(word_count, sizeof(*target_live));
	worklist = zend_mir_value_calloc(
		ssa->cfg.blocks_count, sizeof(*worklist));
	queued = zend_mir_value_calloc(
		ssa->cfg.blocks_count, sizeof(*queued));
	if ((matrix_words != 0
				&& (block_use == NULL || block_def == NULL
					|| block_phi_def == NULL || live_in == NULL
					|| live_out == NULL))
			|| (word_count != 0
				&& (next_out == NULL || next_in == NULL
					|| target_live == NULL))
			|| (ssa->cfg.blocks_count != 0
				&& (worklist == NULL || queued == NULL))) {
		goto done;
	}

	for (block_index = 0;
			block_index < ssa->cfg.blocks_count; block_index++) {
		const zend_basic_block *block = &ssa->cfg.blocks[block_index];
		uint64_t *uses = block_use + (size_t) block_index * word_count;
		uint64_t *defs = block_def + (size_t) block_index * word_count;
		uint64_t *phi_defs =
			block_phi_def + (size_t) block_index * word_count;
		zend_ssa_phi *phi;
		uint32_t op_index;

		if ((block->flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		if (block->start > op_array->last
				|| block->len > op_array->last - block->start) {
			goto done;
		}
		for (phi = ssa->blocks[block_index].phis;
				phi != NULL; phi = phi->next) {
			if (phi->ssa_var < 0
					|| (uint32_t) phi->ssa_var
						>= (uint32_t) ssa->vars_count) {
				goto done;
			}
			zend_mir_bit_set(defs, (uint32_t) phi->ssa_var);
			zend_mir_bit_set(phi_defs, (uint32_t) phi->ssa_var);
		}
		for (op_index = block->start;
				op_index < block->start + block->len; op_index++) {
			const zend_ssa_op *op = &ssa->ops[op_index];
			const int op_uses[3] = {
				op->op1_use, op->op2_use, op->result_use
			};
			const int op_defs[3] = {
				op->op1_def, op->op2_def, op->result_def
			};
			uint32_t operand;

			for (operand = 0; operand < 3; operand++) {
				const int use = op_uses[operand];
				if (!zend_mir_ssa_id_valid(
						use, (uint32_t) ssa->vars_count)) {
					goto done;
				}
				if (use >= 0
						&& (defs[(uint32_t) use / 64]
							& (UINT64_C(1)
								<< ((uint32_t) use % 64))) == 0) {
					zend_mir_bit_set(uses, (uint32_t) use);
				}
			}
			for (operand = 0; operand < 3; operand++) {
				const int def = op_defs[operand];
				if (!zend_mir_ssa_id_valid(
						def, (uint32_t) ssa->vars_count)) {
					goto done;
				}
				if (def >= 0) {
					zend_mir_bit_set(defs, (uint32_t) def);
				}
			}
		}
	}

	{
		uint32_t worklist_count = 0;
		for (block_index = ssa->cfg.blocks_count;
				block_index-- > 0;) {
			if ((ssa->cfg.blocks[block_index].flags
					& ZEND_BB_REACHABLE) != 0) {
				worklist[worklist_count++] = block_index;
				queued[block_index] = 1;
			}
		}
		while (worklist_count != 0) {
			const uint32_t block_number =
				worklist[--worklist_count];
			const zend_basic_block *block =
				&ssa->cfg.blocks[block_number];
			uint64_t *block_live_in =
				live_in + (size_t) block_number * word_count;
			uint64_t *block_live_out =
				live_out + (size_t) block_number * word_count;
			const uint64_t *uses =
				block_use + (size_t) block_number * word_count;
			const uint64_t *defs =
				block_def + (size_t) block_number * word_count;
			bool changed = false;
			uint32_t successor_index;
			uint32_t word;

			queued[block_number] = 0;
			memset(next_out, 0, (size_t) word_count * sizeof(*next_out));
			for (successor_index = 0;
				successor_index < block->successors_count;
				successor_index++) {
				const int successor_number =
					block->successors[successor_index];
				const zend_basic_block *successor;
				const uint64_t *successor_live_in;
				const uint64_t *successor_phi_defs;
				uint32_t predecessor_index;
				zend_ssa_phi *phi;

				if (successor_number < 0
						|| (uint32_t) successor_number
							>= ssa->cfg.blocks_count) {
					goto done;
				}
				successor = &ssa->cfg.blocks[successor_number];
				successor_live_in = live_in
					+ (size_t) successor_number * word_count;
				successor_phi_defs = block_phi_def
					+ (size_t) successor_number * word_count;
				for (word = 0; word < word_count; word++) {
					next_out[word] |=
						successor_live_in[word]
							& ~successor_phi_defs[word];
				}
				if (successor->predecessor_offset < 0) {
					goto done;
				}
				if ((uint32_t) successor->predecessor_offset
							> ssa->cfg.edges_count
						|| successor->predecessors_count
							> ssa->cfg.edges_count
								- (uint32_t)
									successor->predecessor_offset) {
					goto done;
				}
				for (predecessor_index = 0;
					predecessor_index
						< successor->predecessors_count;
					predecessor_index++) {
					if (ssa->cfg.predecessors[
							successor->predecessor_offset
								+ predecessor_index]
							== (int) block_number) {
						break;
					}
				}
				if (predecessor_index
						== successor->predecessors_count) {
					goto done;
				}
				for (phi = ssa->blocks[successor_number].phis;
					phi != NULL; phi = phi->next) {
					const int source =
						phi->sources[predecessor_index];
					if (!zend_mir_ssa_id_valid(
							source,
							(uint32_t) ssa->vars_count)) {
						goto done;
					}
					if (source >= 0) {
						zend_mir_bit_set(
							next_out, (uint32_t) source);
					}
				}
			}
			for (word = 0; word < word_count; word++) {
				next_in[word] =
					uses[word] | (next_out[word] & ~defs[word]);
				if (next_out[word] != block_live_out[word]
						|| next_in[word] != block_live_in[word]) {
					changed = true;
				}
			}
			if (!changed) {
				continue;
			}
			memcpy(block_live_out, next_out,
				(size_t) word_count * sizeof(*block_live_out));
			memcpy(block_live_in, next_in,
				(size_t) word_count * sizeof(*block_live_in));
			if (block->predecessors_count != 0
					&& block->predecessor_offset < 0) {
				goto done;
			}
			if (block->predecessors_count != 0
					&& ((uint32_t) block->predecessor_offset
							> ssa->cfg.edges_count
						|| block->predecessors_count
							> ssa->cfg.edges_count
								- (uint32_t)
									block->predecessor_offset)) {
				goto done;
			}
			for (index = 0;
				index < block->predecessors_count; index++) {
				const int predecessor =
					ssa->cfg.predecessors[
						block->predecessor_offset + index];
				if (predecessor < 0
						|| (uint32_t) predecessor
							>= ssa->cfg.blocks_count) {
					goto done;
				}
				if (queued[predecessor] == 0) {
					queued[predecessor] = 1;
					worklist[worklist_count++] =
						(uint32_t) predecessor;
				}
			}
		}
	}

	for (index = 0; index < target_count; index++) {
		const uint32_t target = targets[index];
		const uint32_t target_block = ssa->cfg.map[target];
		const zend_basic_block *block;
		uint32_t op_index;
		uint32_t value;

		if (target_block >= ssa->cfg.blocks_count) {
			goto done;
		}
		block = &ssa->cfg.blocks[target_block];
		if ((block->flags & ZEND_BB_REACHABLE) == 0
				|| target < block->start
				|| target >= block->start + block->len) {
			goto done;
		}
		memcpy(target_live,
			live_out + (size_t) target_block * word_count,
			(size_t) word_count * sizeof(*target_live));
		for (op_index = block->start + block->len;
			op_index-- > target;) {
			const zend_ssa_op *op = &ssa->ops[op_index];
			const int op_defs[3] = {
				op->op1_def, op->op2_def, op->result_def
			};
			const int op_uses[3] = {
				op->op1_use, op->op2_use, op->result_use
			};
			uint32_t operand;
			for (operand = 0; operand < 3; operand++) {
				if (op_defs[operand] >= 0) {
					zend_mir_bit_reset(
						target_live,
						(uint32_t) op_defs[operand]);
				}
			}
			for (operand = 0; operand < 3; operand++) {
				if (op_uses[operand] >= 0) {
					zend_mir_bit_set(
						target_live,
						(uint32_t) op_uses[operand]);
				}
			}
		}
		for (value = 0;
			value < (uint32_t) ssa->vars_count; value++) {
			zend_mir_suspend_live_value_ref live;
			uint32_t value_index;
			const int storage = ssa->vars[value].var;

			if ((target_live[value / 64]
					& (UINT64_C(1) << (value % 64))) == 0) {
				continue;
			}
			live.target_source_position_id = target;
			live.value_id = zend_mir_value_from_original_ssa(value);
			if (!zend_mir_module_find_value(
					module, live.value_id, &value_index)) {
				continue;
			}
			if (storage < 0 || (uint32_t) storage >= storage_count) {
				goto done;
			}
			live.storage_id = (uint32_t) storage;
			if (!mutator->add_suspend_live_value(
					mutator->context, &live)) {
				goto done;
			}
		}
	}
	success = true;

done:
	free(queued);
	free(worklist);
	free(target_live);
	free(next_in);
	free(next_out);
	free(live_out);
	free(live_in);
	free(block_phi_def);
	free(block_def);
	free(block_use);
	free(targets);
	return success;
}

bool zend_mir_emit_executable_values(
	const zend_op_array *op_array,
	zend_mir_lowering_context *lowering_context,
	zend_mir_module *module,
	const zend_mir_control_flow_map *control_flow_map,
	zend_mir_straight_line_provider_context *frame_context,
	const uint8_t *scalarized_opcodes,
	uint32_t scalarized_opcode_count)
{
	const zend_mir_lowering_source_view *source;
	zend_mir_source_call_view semantic_source;
	const zend_ssa *semantic_ssa = NULL;
	const zend_mir_view *view;
	zend_mir_executable_value_ref *operations;
	zend_mir_value_location_ref *locations;
	zend_mir_instruction_id *control_instruction_by_source;
	zend_mir_block_id *mir_block_by_source = NULL;
	uint32_t *ssa_by_storage;
	uint32_t *argument_by_ssa;
	uint8_t *machine_defined_values;
	uint8_t *expression_throw_tails = NULL;
	zend_mir_value_mutator *value_mutator;
	zend_mir_mutator *mutator;
	uint32_t operation_count = 0;
	uint32_t location_count = 0;
	uint32_t source_ssa_count;
	uint32_t source_block_count;
	uint32_t storage_count;
	uint32_t value_count;
	uint32_t index;
	bool success = false;

	if (op_array == NULL || lowering_context == NULL || module == NULL
			|| control_flow_map == NULL || frame_context == NULL
			|| op_array->last > ZEND_MIR_LIMIT
			|| (scalarized_opcodes != NULL
				&& scalarized_opcode_count != op_array->last)) {
		return false;
	}
	source = lowering_context->source;
	view = lowering_context->module_ops.view(
		lowering_context->module_ops.context, module);
	if (source == NULL || view == NULL || source->ssa_count == NULL
			|| source->ssa_at == NULL || source->block_count == NULL
			|| source->block_at == NULL
			|| source->opcode_count(source->context) != op_array->last
			|| lowering_context->zend_source == NULL
			|| !zend_mir_zend_source_call_view(
				lowering_context->zend_source, &semantic_source)
			|| semantic_source.source_opcode_count == NULL
			|| semantic_source.source_opcode_at == NULL
			|| semantic_source.source_opcode_count(
				semantic_source.context) != op_array->last) {
		return false;
	}
	source_ssa_count = source->ssa_count(source->context);
	source_block_count = source->block_count(source->context);
	semantic_ssa = (const zend_ssa *) lowering_context->zend_source->ssa;
	if (semantic_ssa == NULL || semantic_ssa->ops == NULL
			|| semantic_ssa->vars_count < 0
			|| (uint32_t) semantic_ssa->vars_count != source_ssa_count
			|| (source_ssa_count != 0 && semantic_ssa->vars == NULL)) {
		return false;
	}
	if (source_ssa_count > ZEND_MIR_LIMIT
			|| (uint32_t) op_array->last_var
				> ZEND_MIR_LIMIT - op_array->T) {
		return false;
	}
	storage_count = (uint32_t) op_array->last_var + op_array->T;
	value_count = view->value_count(view->context);
	if (value_count > ZEND_MIR_LIMIT) {
		return false;
	}
	operations = zend_mir_value_calloc(op_array->last, sizeof(*operations));
	if (op_array->last != 0 && operations == NULL) {
		return false;
	}
	locations = zend_mir_value_calloc(source_ssa_count, sizeof(*locations));
	if (source_ssa_count != 0 && locations == NULL) {
		free(operations);
		return false;
	}
	argument_by_ssa = zend_mir_value_calloc(
		source_ssa_count, sizeof(*argument_by_ssa));
	if (source_ssa_count != 0 && argument_by_ssa == NULL) {
		free(locations);
		free(operations);
		return false;
	}
	control_instruction_by_source = zend_mir_value_calloc(
		op_array->last, sizeof(*control_instruction_by_source));
	if (op_array->last != 0 && control_instruction_by_source == NULL) {
		free(argument_by_ssa);
		free(locations);
		free(operations);
		return false;
	}
	ssa_by_storage = zend_mir_value_calloc(
		storage_count, sizeof(*ssa_by_storage));
	if (storage_count != 0 && ssa_by_storage == NULL) {
		free(control_instruction_by_source);
		free(argument_by_ssa);
		free(locations);
		free(operations);
		return false;
	}
	for (index = 0; index < storage_count; index++) {
		ssa_by_storage[index] = ZEND_MIR_ID_INVALID;
	}
	machine_defined_values = zend_mir_value_calloc(
		value_count, sizeof(*machine_defined_values));
	if (value_count != 0 && machine_defined_values == NULL) {
		free(ssa_by_storage);
		free(control_instruction_by_source);
		free(argument_by_ssa);
		free(locations);
		free(operations);
		return false;
	}
	mir_block_by_source = zend_mir_value_calloc(
		source_block_count, sizeof(*mir_block_by_source));
	if (source_block_count != 0 && mir_block_by_source == NULL) {
		goto done;
	}
	if (source_block_count != 0
			&& !zend_mir_build_block_index(source, control_flow_map, view,
				mir_block_by_source, source_block_count)) {
		goto done;
	}
	if (op_array->last != 0) {
		expression_throw_tails = zend_mir_value_calloc(
			op_array->last, sizeof(*expression_throw_tails));
		if (expression_throw_tails == NULL
				|| !zend_mir_mark_expression_throw_tails(
					op_array, semantic_ssa, expression_throw_tails)) {
			goto done;
		}
	}
	for (index = 0; index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record instruction;
		uint32_t value_index;

		if (!view->instruction_at(view->context, index, &instruction)) {
			goto done;
		}
		if (zend_mir_id_is_valid(instruction.result_id)
				&& zend_mir_module_find_value(
					module, instruction.result_id, &value_index)) {
			machine_defined_values[value_index] = 1;
		}
	}
	for (index = 0; index < view->constant_count(view->context); index++) {
		zend_mir_constant_record constant;
		uint32_t value_index;

		if (!view->constant_at(view->context, index, &constant)
				|| !zend_mir_module_find_value(
					module, constant.value_id, &value_index)) {
			goto done;
		}
		machine_defined_values[value_index] = 1;
	}
	if (op_array->last != 0
			&& !zend_mir_index_control_value_instructions(
				view, op_array->last, control_instruction_by_source)) {
		goto done;
	}
	if (source_ssa_count != 0
			&& !zend_mir_index_frame_arguments(
				op_array, &semantic_source, source_ssa_count,
				argument_by_ssa)) {
		goto done;
	}
	mutator = lowering_context->module_ops.mutator(
		lowering_context->module_ops.context, module);
	value_mutator = zend_mir_module_get_value_mutator(module);
	if (mutator == NULL || value_mutator == NULL
			|| value_mutator->set_model_flags == NULL
			|| value_mutator->add_value_location == NULL
			|| value_mutator->add_executable_operation == NULL
			|| value_mutator->add_suspend_live_value == NULL
			|| !value_mutator->set_model_flags(
				value_mutator->context,
				ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS)) {
		goto done;
	}
	for (index = 0; index < source_ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_id value_id;
		zend_mir_storage_id storage_id;
		uint32_t type = semantic_ssa != NULL
				&& semantic_ssa->var_info != NULL
			? semantic_ssa->var_info[index].type : 0;
		bool alias_observable =
			(type & (MAY_BE_REF | MAY_BE_INDIRECT)) != 0;
		uint32_t value_index;

		if (!source->ssa_at(source->context, index, &ssa)) {
			goto done;
		}
		value_id = zend_mir_value_from_original_ssa(ssa.ssa_variable_id);
		if (!zend_mir_module_find_value(module, value_id, &value_index)) {
			continue;
		}
		storage_id = zend_mir_ssa_storage_id(op_array, &ssa);
		if (!zend_mir_id_is_valid(storage_id)) {
			goto done;
		}
		locations[location_count].value_id = value_id;
		locations[location_count].storage_id = storage_id;
		locations[location_count].frame_argument_ordinal_plus_one =
			argument_by_ssa[ssa.ssa_variable_id];
		if (locations[location_count].frame_argument_ordinal_plus_one != 0
				&& ARG_SHOULD_BE_SENT_BY_REF(
					(const zend_function *) op_array,
					locations[location_count]
						.frame_argument_ordinal_plus_one)) {
			alias_observable = true;
		}
		locations[location_count].alias_observable = alias_observable;
		locations[location_count].category = alias_observable
			? ZEND_MIR_VALUE_REFERENCE_CELL
			: zend_mir_type_category(type);
		locations[location_count].refcount_state =
			zend_mir_type_refcount_state(
				type, locations[location_count].category);
		location_count++;
	}
	for (index = 0; index < op_array->last; index++) {
		zend_mir_source_opcode_ref source_opcode;
		zend_mir_source_block_ref source_block;
		zend_mir_executable_value_ref *operation;
		zend_mir_safepoint_class frame_class;
		zend_mir_opcode opcode = zend_mir_executable_opcode(
			op_array->opcodes[index].opcode);

		/* Every definition of a temporary, a call result included, replaces
		 * the identity a reused slot carries. */
		if (semantic_ssa != NULL
				&& (op_array->opcodes[index].result_type
					& (IS_TMP_VAR | IS_VAR)) != 0
				&& semantic_ssa->ops[index].result_def >= 0
				&& EX_VAR_TO_NUM(op_array->opcodes[index].result.var)
					< storage_count) {
			ssa_by_storage[EX_VAR_TO_NUM(
				op_array->opcodes[index].result.var)] =
					(uint32_t) semantic_ssa->ops[index].result_def;
		}
		if (expression_throw_tails != NULL
				&& expression_throw_tails[index]) {
			continue;
		}
		/*
		 * Scalar lowering already owns the complete semantics and dataflow
		 * for these source operations. Emitting the executable-zval
		 * operation as well would replay the same assignment or increment
		 * against the Zend frame after the machine SSA result was produced.
		 */
		if (scalarized_opcodes != NULL && scalarized_opcodes[index]) {
			continue;
		}
		if (opcode == ZEND_MIR_OPCODE_INVALID) {
			opcode = zend_mir_control_value_opcode(
				op_array->opcodes[index].opcode);
		}
		if (opcode == ZEND_MIR_OPCODE_INVALID) {
			continue;
		}
		if (!semantic_source.source_opcode_at(
				semantic_source.context, index, &source_opcode)
				|| source_opcode.opline_index != index
				|| source_opcode.block_id >= source_block_count
				|| !zend_mir_value_source_block(
					source, source_opcode.block_id, &source_block)) {
			goto done;
		}
		if ((source_block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		operation = &operations[operation_count];
		memset(operation, 0, sizeof(*operation));
		operation->id = ZEND_MIR_ID_INVALID;
		operation->opcode = opcode;
		operation->source_opcode = source_opcode.zend_opcode_number;
		operation->op1 = source_opcode.op1;
		operation->op2 = source_opcode.op2;
		operation->result = source_opcode.result;
		operation->op1_unused_payload =
			source_opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				? op_array->opcodes[index].op1.num : 0;
		operation->op2_unused_payload =
			source_opcode.op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				? op_array->opcodes[index].op2.num : 0;
		operation->result_unused_payload =
			source_opcode.result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				? op_array->opcodes[index].result.num : 0;
		operation->op1_storage_id = zend_mir_operand_storage_id(
			op_array, &source_opcode.op1);
		operation->op2_storage_id = zend_mir_operand_storage_id(
			op_array, &source_opcode.op2);
		operation->result_storage_id = zend_mir_operand_storage_id(
			op_array, &source_opcode.result);
		if (semantic_ssa != NULL && semantic_ssa->ops[index].op1_def >= 0) {
			const uint32_t definition =
				(uint32_t) semantic_ssa->ops[index].op1_def;
			const int definition_storage =
				definition < source_ssa_count
					? semantic_ssa->vars[definition].var : -1;

			if (definition >= source_ssa_count || definition == UINT32_MAX
					|| definition_storage < 0
					|| (uint32_t) definition_storage >= storage_count
					|| operation->op1_storage_id
						!= (uint32_t) definition_storage) {
				goto done;
			}
			operation->op1_definition_ssa_variable_id_plus_one =
				definition + 1;
		}
		if (opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& !zend_mir_id_is_valid(
					operation->op1.ssa_variable_id)
				&& zend_mir_id_is_valid(operation->op1_storage_id)
				&& operation->op1_storage_id < storage_count
				&& zend_mir_id_is_valid(
					ssa_by_storage[operation->op1_storage_id])) {
			const bool verified = index > 0
				&& op_array->opcodes[index - 1].opcode
					== ZEND_VERIFY_RETURN_TYPE;
			const int use = semantic_ssa != NULL && !verified
				? semantic_ssa->ops[index].op1_use : -1;

			/*
			 * VERIFY_RETURN_TYPE and the following RETURN share the same
			 * physical carrier, but Zend SSA intentionally omits the second
			 * use. Preserve the last explicit source-backed SSA identity so
			 * the attached return descriptor remains exact and pointer-free.
			 * Any other RETURN keeps its own use: a join such as the result of
			 * `$a && $b` is a PHI, not the last definition in opline order.
			 */
			operation->op1.ssa_variable_id =
				use >= 0 && (uint32_t) use < source_ssa_count
					&& semantic_ssa->vars[use].var
						== (int) operation->op1_storage_id
				? (uint32_t) use
				: ssa_by_storage[operation->op1_storage_id];
		}
		operation->auxiliary.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
		operation->auxiliary.slot_kind =
			ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		operation->auxiliary.index = ZEND_MIR_ID_INVALID;
		operation->auxiliary.ssa_variable_id = ZEND_MIR_ID_INVALID;
		operation->auxiliary_unused_payload = 0;
		operation->auxiliary_storage_id = ZEND_MIR_ID_INVALID;
		if (index + 1 < op_array->last
				&& op_array->opcodes[index + 1].opcode == ZEND_OP_DATA) {
			zend_mir_source_opcode_ref data_opcode;

			if (!semantic_source.source_opcode_at(
					semantic_source.context, index + 1, &data_opcode)
					|| data_opcode.zend_opcode_number != ZEND_OP_DATA) {
				goto done;
			}
			operation->auxiliary = data_opcode.op1;
			operation->auxiliary_unused_payload =
				data_opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
					? op_array->opcodes[index + 1].op1.num : 0;
			operation->auxiliary_storage_id =
				zend_mir_operand_storage_id(
					op_array, &data_opcode.op1);
		}
		operation->extended_value = source_opcode.extended_value;
		operation->source_position_id = source_opcode.source_position_id;
		operation->frame_state_id = ZEND_MIR_ID_INVALID;
		operation->block_id = mir_block_by_source[source_opcode.block_id];
		if (!zend_mir_id_is_valid(operation->block_id)
				|| !zend_mir_operation_semantics(
					opcode, operation, &frame_class)) {
			goto done;
		}
		if ((opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
				|| opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
				|| opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				|| opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL)
				&& (operation->source_position_id >= op_array->last
					|| !zend_mir_id_is_valid(
						control_instruction_by_source[
							operation->source_position_id]))) {
			if (opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
				continue;
			}
			goto done;
		}
		if (opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
				|| opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
				|| opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				|| opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			operation->id = control_instruction_by_source[
				operation->source_position_id];
		}
		if (opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
			zend_mir_instruction_record branch_instruction;

			if (!view->instruction_at(
					view->context, operation->id, &branch_instruction)) {
				goto done;
			}
			if (branch_instruction.opcode == ZEND_MIR_OPCODE_COND_BRANCH) {
				zend_mir_value_id condition_id;
				zend_mir_value_record condition;
				uint32_t condition_index;
				bool machine_condition;

				/*
				 * COND_BRANCH describes topology, not the condition carrier.
				 * A real non-zval MIR operand is already machine-defined and
				 * must remain on the direct branch path. Control flow whose
				 * condition is a canonical source zval uses the same
				 * topology; only that case needs the explicit source
				 * operation.
				 */
				machine_condition =
					view->instruction_operand_count(
						view->context, branch_instruction.id) == 1
					&& view->instruction_operand_at(
						view->context, branch_instruction.id, 0,
						&condition_id)
					&& zend_mir_module_find_value(
						module, condition_id, &condition_index)
					&& machine_defined_values[condition_index]
					&& view->value_at(
						view->context, condition_index, &condition)
					&& condition.id == condition_id
					&& condition.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					&& condition.representation
						!= ZEND_MIR_REPRESENTATION_VOID
					&& condition.representation
						!= ZEND_MIR_REPRESENTATION_CONTROL;
				if (machine_condition) {
					continue;
				}
			}
			if (branch_instruction.opcode != ZEND_MIR_OPCODE_COND_BRANCH
					&& branch_instruction.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				goto done;
			}
		}
		if (opcode == ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH) {
			zend_mir_instruction_record branch_instruction;

			if (!view->instruction_at(
					view->context, operation->id, &branch_instruction)
					|| branch_instruction.opcode
						!= ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH) {
				goto done;
			}
		}
		if (opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| opcode == ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH) {
			zend_mir_instruction_record branch_instruction;

			if (!view->instruction_at(
					view->context, operation->id, &branch_instruction)
					|| branch_instruction.opcode != opcode) {
				goto done;
			}
		}
		if (opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL) {
			zend_mir_instruction_record throw_instruction;

			if (!view->instruction_at(
					view->context, operation->id, &throw_instruction)
					|| throw_instruction.opcode
						!= ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| !zend_mir_id_is_valid(
						throw_instruction.frame_state_id)) {
				goto done;
			}
			operation->frame_state_id = throw_instruction.frame_state_id;
			frame_class = ZEND_MIR_SAFEPOINT_CLASS_INVALID;
		}
		if (frame_class != ZEND_MIR_SAFEPOINT_CLASS_INVALID) {
			zend_mir_source_position_id emitted_source;
			if (!zend_mir_straight_line_emit_frame_for_class(
					lowering_context, &source_opcode, mutator, frame_context,
					frame_class, &operation->frame_state_id, &emitted_source)
					|| emitted_source != operation->source_position_id) {
				goto done;
			}
		}
		if (zend_mir_id_is_valid(operation->op1_storage_id)
				&& operation->op1_storage_id < storage_count
				&& zend_mir_id_is_valid(
					operation->op1.ssa_variable_id)) {
			ssa_by_storage[operation->op1_storage_id] =
				operation->op1.ssa_variable_id;
		}
		if (zend_mir_id_is_valid(operation->op2_storage_id)
				&& operation->op2_storage_id < storage_count
				&& zend_mir_id_is_valid(
					operation->op2.ssa_variable_id)) {
			ssa_by_storage[operation->op2_storage_id] =
				operation->op2.ssa_variable_id;
		}
		if (zend_mir_id_is_valid(operation->result_storage_id)
				&& operation->result_storage_id < storage_count
				&& zend_mir_id_is_valid(
					operation->result.ssa_variable_id)) {
			ssa_by_storage[operation->result_storage_id] =
				operation->result.ssa_variable_id;
		}
		operation_count++;
	}
	if (location_count != 0) {
		qsort(locations, location_count, sizeof(*locations),
			zend_mir_compare_value_locations);
	}
	for (index = 0; index < location_count; index++) {
		if (!value_mutator->add_value_location(
				value_mutator->context, &locations[index])) {
			goto done;
		}
	}
	if (!zend_mir_freeze_suspend_liveness(
				op_array, semantic_ssa, module, value_mutator)) {
		goto done;
	}
	if (operation_count != 0) {
		qsort(operations, operation_count, sizeof(*operations),
			zend_mir_compare_operations);
	}
	for (index = 0; index < operation_count; index++) {
		if (!value_mutator->add_executable_operation(
				value_mutator->context, &operations[index])) {
			goto done;
		}
	}
	success = zend_mir_module_commit_value_model(module);

done:
	free(mir_block_by_source);
	free(expression_throw_tails);
	free(machine_defined_values);
	free(ssa_by_storage);
	free(control_instruction_by_source);
	free(argument_by_ssa);
	free(locations);
	free(operations);
	return success;
}

static zend_mir_value_category zend_mir_type_category(uint32_t type)
{
	const uint32_t scalar_types =
		MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE
		| MAY_BE_LONG | MAY_BE_DOUBLE;
	uint32_t concrete = type & MAY_BE_ANY;
	uint32_t categories = 0;

	/*
	 * UNDEF and refcount/cardinality bits qualify a concrete value; they do
	 * not create another value category. Multiple scalar alternatives remain
	 * one non-refcounted category, while a scalar/refcounted union stays
	 * deliberately unknown.
	 */
	if ((concrete & scalar_types) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if ((concrete & MAY_BE_STRING) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_STRING;
	}
	if ((concrete & MAY_BE_ARRAY) != 0) {
		categories |=
			UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
	}
	if ((concrete & MAY_BE_OBJECT) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_OBJECT_ABSTRACT;
	}
	if ((concrete & MAY_BE_RESOURCE) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_RESOURCE_ABSTRACT;
	}
	if (categories == 0 && (type & MAY_BE_UNDEF) != 0) {
		categories =
			UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if (categories == 0 || (categories & (categories - 1)) != 0) {
		return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR)) != 0) {
		return ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_STRING)) != 0) {
		return ZEND_MIR_VALUE_REFCOUNTED_STRING;
	}
	if ((categories
			& (UINT32_C(1)
				<< ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT)) != 0) {
		return ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_OBJECT_ABSTRACT)) != 0) {
		return ZEND_MIR_VALUE_OBJECT_ABSTRACT;
	}
	return ZEND_MIR_VALUE_RESOURCE_ABSTRACT;
}
