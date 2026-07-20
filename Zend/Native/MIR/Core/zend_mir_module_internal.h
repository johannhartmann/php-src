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

#ifndef ZEND_MIR_CORE_MODULE_INTERNAL_H
#define ZEND_MIR_CORE_MODULE_INTERNAL_H

#include <stdalign.h>
#include <string.h>

#include "zend_mir_arena.h"
#include "../zend_mir_call.h"

typedef struct _zend_mir_core_function {
	zend_mir_function_record record;
	bool sealed;
} zend_mir_core_function;

typedef struct _zend_mir_core_instruction {
	zend_mir_instruction_record record;
	zend_mir_value_id *operands;
	uint32_t operand_count;
	uint32_t operand_capacity;
} zend_mir_core_instruction;

typedef struct _zend_mir_core_value {
	zend_mir_value_record record;
	uint32_t constant_index;
} zend_mir_core_value;

typedef struct _zend_mir_core_edge {
	zend_mir_block_id from;
	zend_mir_block_id to;
} zend_mir_core_edge;

typedef struct _zend_mir_core_table {
	void *items;
	uint32_t count;
	uint32_t capacity;
} zend_mir_core_table;

typedef struct _zend_mir_core_call_staging {
	zend_mir_call_target_ref *targets;
	zend_mir_call_argument_ref *arguments;
	zend_mir_call_continuation_ref *continuations;
	zend_mir_call_site_ref *sites;
	uint32_t target_count;
	uint32_t target_capacity;
	uint32_t argument_count;
	uint32_t argument_capacity;
	uint32_t continuation_count;
	uint32_t continuation_capacity;
	uint32_t site_count;
	uint32_t site_capacity;
	bool committed;
} zend_mir_core_call_staging;

struct _zend_mir_module {
	zend_mir_module_id id;
	zend_mir_module_state state;
	zend_mir_core_limits limits;
	zend_mir_arena arena;
	zend_mir_diagnostic_sink *diagnostics;
	zend_mir_core_table functions;
	zend_mir_core_table blocks;
	zend_mir_core_table edges;
	zend_mir_core_table instructions;
	zend_mir_core_table values;
	zend_mir_core_table constants;
	zend_mir_core_table value_facts;
	zend_mir_core_table frame_states;
	zend_mir_core_table source_positions;
	zend_mir_core_table frame_slots;
	zend_mir_core_table roots;
	zend_mir_core_table cleanups;
	zend_mir_core_table source_maps;
	zend_mir_core_table call_targets;
	zend_mir_core_table call_arguments;
	zend_mir_core_table call_continuations;
	zend_mir_core_table call_sites;
	zend_mir_core_table call_receipts;
	zend_mir_core_table capability_ids;
	zend_mir_core_table semantic_debt_ids;
	zend_mir_core_table verifier_receipts;
	zend_mir_core_call_staging call_staging;
	uint32_t operand_count;
	uint32_t *value_index;
	uint32_t value_index_capacity;
	zend_mir_view view;
	zend_mir_mutator mutator;
	zend_mir_call_view call_view;
	zend_mir_call_mutator call_mutator;
};

bool zend_mir_core_next_id(uint32_t count, uint32_t *out);
uint32_t zend_mir_core_hash_id(uint32_t id);

bool zend_mir_module_fail(zend_mir_module *module, zend_mir_diagnostic_code code,
	const char *message);
bool zend_mir_module_require_building(zend_mir_module *module);
bool zend_mir_module_grow_table(zend_mir_module *module, zend_mir_core_table *table,
	size_t item_size, size_t item_alignment, uint32_t limit);
bool zend_mir_module_find_value(const zend_mir_module *module, zend_mir_value_id id,
	uint32_t *index_out);
bool zend_mir_module_prepare_value_index(zend_mir_module *module);
void zend_mir_module_insert_value_index(zend_mir_module *module,
	zend_mir_value_id id, uint32_t index);
bool zend_mir_core_add_value_fact(void *context,
	const zend_mir_value_fact_ref *requested, zend_mir_value_fact_id *out);

void zend_mir_module_init_view(zend_mir_module *module);
void zend_mir_module_init_mutator(zend_mir_module *module);
void zend_mir_module_init_call_view(zend_mir_module *module);
void zend_mir_module_init_call_mutator(zend_mir_module *module);

zend_mir_call_mutator *zend_mir_module_get_call_mutator(
	zend_mir_module *module);
const zend_mir_call_view *zend_mir_module_get_call_view(
	const zend_mir_module *module);
bool zend_mir_module_w05_capability_ids_are_canonical(
	const zend_mir_module *module);
enum {
	ZEND_MIR_W05_VERIFIED_STRUCTURAL = UINT32_C(1) << 0,
	ZEND_MIR_W05_VERIFIED_SCALAR = UINT32_C(1) << 1,
	ZEND_MIR_W05_VERIFIED_CONTROL_FLOW = UINT32_C(1) << 2,
	ZEND_MIR_W05_VERIFIED_CALL_MODEL = UINT32_C(1) << 3,
	ZEND_MIR_W05_VERIFIED_ALL =
		ZEND_MIR_W05_VERIFIED_STRUCTURAL
		| ZEND_MIR_W05_VERIFIED_SCALAR
		| ZEND_MIR_W05_VERIFIED_CONTROL_FLOW
		| ZEND_MIR_W05_VERIFIED_CALL_MODEL
};
bool zend_mir_module_publish_w05_verifier_receipts(
	zend_mir_module *module,
	const uint32_t module_fingerprint[4],
	const uint32_t source_fingerprint[4],
	uint32_t verified_facets);
bool zend_mir_dump_w05_fingerprint_projection(
	const zend_mir_view *view, zend_mir_text_writer *writer,
	zend_mir_diagnostic_sink *diagnostics);
static inline const zend_mir_call_view *
zend_mir_module_call_view_from_view(const zend_mir_view *view)
{
	const zend_mir_module *module;
	uintptr_t view_address;

	if (view == NULL || view->context == NULL) {
		return NULL;
	}
	view_address = (uintptr_t) (const void *) view;
	if (view_address < offsetof(zend_mir_module, view)) {
		return NULL;
	}
	module = (const zend_mir_module *)
		(view_address - offsetof(zend_mir_module, view));
	return (const void *) module == view->context
		&& module->state != ZEND_MIR_MODULE_FAILED
		&& module->call_staging.committed
		? &module->call_view : NULL;
}

#define ZEND_MIR_CORE_ITEMS(module, field, type) \
	((type *) (module)->field.items)

#endif /* ZEND_MIR_CORE_MODULE_INTERNAL_H */
