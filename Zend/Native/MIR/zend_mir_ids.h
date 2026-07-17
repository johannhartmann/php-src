/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_IDS_H
#define ZEND_MIR_IDS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
# define ZEND_MIR_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
# define ZEND_MIR_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define ZEND_MIR_CONTRACT_VERSION_MAJOR UINT32_C(1)
#define ZEND_MIR_CONTRACT_VERSION_MINOR UINT32_C(0)
#define ZEND_MIR_CONTRACT_VERSION \
	((ZEND_MIR_CONTRACT_VERSION_MAJOR << 16) | ZEND_MIR_CONTRACT_VERSION_MINOR)

#define ZEND_MIR_ID_INVALID UINT32_C(0xffffffff)
#define ZEND_MIR_ID_MAX UINT32_C(0xfffffffe)

#define ZEND_MIR_VALUE_SYNTHETIC_BIT UINT32_C(0x80000000)
#define ZEND_MIR_VALUE_ORIGINAL_MAX UINT32_C(0x7fffffff)
#define ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX UINT32_C(0x7ffffffe)
#define ZEND_MIR_VALUE_SYNTHETIC_MAX UINT32_C(0xfffffffe)

typedef uint32_t zend_mir_module_id;
typedef uint32_t zend_mir_function_id;
typedef uint32_t zend_mir_block_id;
typedef uint32_t zend_mir_instruction_id;
typedef uint32_t zend_mir_value_id;
typedef uint32_t zend_mir_frame_state_id;
typedef uint32_t zend_mir_source_position_id;
typedef uint32_t zend_mir_resume_id;
typedef uint32_t zend_mir_symbol_id;

static inline bool zend_mir_id_is_valid(uint32_t id)
{
	return id != ZEND_MIR_ID_INVALID;
}

static inline bool zend_mir_value_is_original_ssa(zend_mir_value_id id)
{
	return id <= ZEND_MIR_VALUE_ORIGINAL_MAX;
}

static inline bool zend_mir_value_is_synthetic(zend_mir_value_id id)
{
	return id != ZEND_MIR_ID_INVALID && (id & ZEND_MIR_VALUE_SYNTHETIC_BIT) != 0;
}

static inline zend_mir_value_id zend_mir_value_from_original_ssa(uint32_t ssa_id)
{
	return ssa_id <= ZEND_MIR_VALUE_ORIGINAL_MAX ? ssa_id : ZEND_MIR_ID_INVALID;
}

static inline zend_mir_value_id zend_mir_value_from_synthetic(uint32_t payload)
{
	return payload <= ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX
		? payload | ZEND_MIR_VALUE_SYNTHETIC_BIT
		: ZEND_MIR_ID_INVALID;
}

ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_module_id) == 4, "MIR IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_value_id) == 4, "MIR value IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_VALUE_SYNTHETIC_MAX < ZEND_MIR_ID_INVALID,
	"invalid value ID is outside both value namespaces");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_VALUE_ORIGINAL_MAX + UINT32_C(1) == ZEND_MIR_VALUE_SYNTHETIC_BIT,
	"original and synthetic value namespaces are adjacent");

#endif /* ZEND_MIR_IDS_H */
