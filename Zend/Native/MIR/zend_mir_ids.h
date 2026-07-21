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
#define ZEND_MIR_CONTRACT_VERSION_MINOR UINT32_C(2)
#define ZEND_MIR_CONTRACT_VERSION \
	((ZEND_MIR_CONTRACT_VERSION_MAJOR << 16) | ZEND_MIR_CONTRACT_VERSION_MINOR)

/*
 * W03 remains frozen at MIR contract 1.2. The additive W04 source/control-flow
 * boundary is 1.3 and is negotiated independently until W04 implementation
 * replaces the W03-only entry point.
 */
#define ZEND_MIR_W04_CONTRACT_VERSION_MINOR UINT32_C(3)
#define ZEND_MIR_W04_CONTRACT_VERSION \
	((ZEND_MIR_CONTRACT_VERSION_MAJOR << 16) | ZEND_MIR_W04_CONTRACT_VERSION_MINOR)

/*
 * W05 adds pointer-free call-model tables without changing W01-W04 records.
 * Minor 1.5 distinguishes final-module guarantees from prerequisite W04
 * verification and gives unlowered callees a stable declaration identity. Minor
 * 1.6 adds an original-opcode proof table to the process-local call source
 * view so W03 projection cannot erase the W05 call-sequence proof. Minor 1.7
 * carries compiler-preserved named-argument syntax into the pointer-free call
 * source view even when Zend normalizes the argument position. Minor 1.8 adds
 * the exact mapped scalar result ID to each immutable MIR call site. Minor 1.9
 * removes persisted capability and verifier records; verification is performed
 * directly before the module is returned.
 */
#define ZEND_MIR_W05_CONTRACT_VERSION_MINOR UINT32_C(9)
#define ZEND_MIR_W05_CONTRACT_VERSION \
	((ZEND_MIR_CONTRACT_VERSION_MAJOR << 16) | ZEND_MIR_W05_CONTRACT_VERSION_MINOR)

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
typedef uint32_t zend_mir_source_map_id;
typedef uint32_t zend_mir_value_fact_id;
typedef uint32_t zend_mir_op_array_id;
typedef uint32_t zend_mir_resume_id;
typedef uint32_t zend_mir_symbol_id;
typedef uint32_t zend_mir_source_block_id;
typedef uint32_t zend_mir_source_edge_id;
typedef uint32_t zend_mir_source_phi_id;
typedef uint32_t zend_mir_call_site_id;
typedef uint32_t zend_mir_call_argument_id;
typedef uint32_t zend_mir_call_target_id;
typedef uint32_t zend_mir_call_continuation_id;
typedef uint32_t zend_mir_source_call_site_id;
typedef uint32_t zend_mir_source_call_argument_id;
typedef uint32_t zend_mir_source_call_target_id;

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
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_source_block_id) == 4,
	"source block IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_source_edge_id) == 4,
	"source edge IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_source_phi_id) == 4,
	"source PHI IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_call_site_id) == 4,
	"call-site IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(sizeof(zend_mir_source_call_site_id) == 4,
	"source call-site IDs are 32-bit");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_VALUE_SYNTHETIC_MAX < ZEND_MIR_ID_INVALID,
	"invalid value ID is outside both value namespaces");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_VALUE_ORIGINAL_MAX + UINT32_C(1) == ZEND_MIR_VALUE_SYNTHETIC_BIT,
	"original and synthetic value namespaces are adjacent");

#endif /* ZEND_MIR_IDS_H */
