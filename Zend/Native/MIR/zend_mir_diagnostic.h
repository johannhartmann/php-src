/* Bounded, deterministic ZNMIR diagnostic contract. */

#ifndef ZEND_MIR_DIAGNOSTIC_H
#define ZEND_MIR_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zend_mir_ids.h"

#define ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY 192

typedef enum _zend_mir_diagnostic_severity {
	ZEND_MIR_DIAGNOSTIC_NOTE = 0,
	ZEND_MIR_DIAGNOSTIC_ERROR = 1,
	ZEND_MIR_DIAGNOSTIC_FATAL = 2,
	ZEND_MIR_DIAGNOSTIC_SEVERITY_INVALID = -1
} zend_mir_diagnostic_severity;

typedef enum _zend_mir_diagnostic_code {
	ZEND_MIR_DIAGNOSTIC_NONE = 0,
	ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED = 1,
	ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED = 2,
	ZEND_MIR_DIAGNOSTIC_INVALID_ID = 3,
	ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID = 4,
	ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE = 5,
	ZEND_MIR_DIAGNOSTIC_INVALID_CFG = 6,
	ZEND_MIR_DIAGNOSTIC_INVALID_PHI = 7,
	ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS = 8,
	ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP = 9,
	ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE = 10,
	ZEND_MIR_DIAGNOSTIC_INVALID_TEXT = 11,
	ZEND_MIR_DIAGNOSTIC_UNSUPPORTED_CONTRACT_VERSION = 12,
	ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS = 13,
	ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT = 14,
	ZEND_MIR_DIAGNOSTIC_INVALID_SCALAR_PROFILE = 15,
	ZEND_MIR_DIAGNOSTIC_COUNT = 16,
	ZEND_MIR_DIAGNOSTIC_CODE_INVALID = -1
} zend_mir_diagnostic_code;

typedef struct _zend_mir_diagnostic_location {
	zend_mir_module_id module_id;
	zend_mir_function_id function_id;
	zend_mir_block_id block_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_frame_state_id frame_state_id;
	zend_mir_source_position_id source_position_id;
} zend_mir_diagnostic_location;

typedef struct _zend_mir_diagnostic {
	zend_mir_diagnostic_code code;
	zend_mir_diagnostic_severity severity;
	zend_mir_diagnostic_location location;
	char message[ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY];
} zend_mir_diagnostic;

typedef bool (*zend_mir_diagnostic_emit_fn)(void *context, const zend_mir_diagnostic *diagnostic);

/* Process-local callback state. It is never serialized as MIR identity. */
typedef struct _zend_mir_diagnostic_sink {
	void *context;
	zend_mir_diagnostic_emit_fn emit;
	uint32_t limit;
	uint32_t emitted;
} zend_mir_diagnostic_sink;

ZEND_MIR_STATIC_ASSERT(ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY == 192,
	"diagnostic message capacity is part of contract version 1");

static inline bool zend_mir_diagnostic_sink_emit(
		zend_mir_diagnostic_sink *sink, const zend_mir_diagnostic *diagnostic)
{
	bool accepted;

	if (sink == NULL || diagnostic == NULL || sink->emit == NULL || sink->emitted >= sink->limit) {
		return false;
	}
	accepted = sink->emit(sink->context, diagnostic);
	sink->emitted++;
	return accepted;
}

#endif /* ZEND_MIR_DIAGNOSTIC_H */
