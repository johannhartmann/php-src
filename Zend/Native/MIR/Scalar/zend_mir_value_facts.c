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

#include "zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"

bool zend_mir_core_add_value_fact(void *context,
		const zend_mir_value_fact_ref *requested, zend_mir_value_fact_id *out)
{
	zend_mir_module *module = (zend_mir_module *) context;
	zend_mir_value_fact_ref *facts;
	zend_mir_value_fact_ref fact;
	zend_mir_core_value *values;
	zend_mir_value_fact_id id;
	uint32_t value_index;
	uint32_t index;

	if (!zend_mir_module_require_building(module) || requested == NULL
			|| out == NULL || !zend_mir_scalar_fact_is_well_formed(requested)
			|| !zend_mir_module_find_value(module, requested->value_id, &value_index)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid scalar value fact");
	}
	values = ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);
	if (values[value_index].record.representation
			!= zend_mir_scalar_type_representation(requested->exact_type)) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT,
			"value fact exact type does not match value representation");
	}
	facts = ZEND_MIR_CORE_ITEMS(module, value_facts, zend_mir_value_fact_ref);
	for (index = 0; index < module->value_facts.count; index++) {
		if (facts[index].value_id == requested->value_id) {
			return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID,
				"value already has a scalar fact");
		}
	}
	if (!zend_mir_module_grow_table(module, &module->value_facts,
			sizeof(zend_mir_value_fact_ref), alignof(zend_mir_value_fact_ref),
			module->limits.values)
			|| !zend_mir_core_next_id(module->value_facts.count, &id)) {
		return false;
	}
	fact = *requested;
	fact.id = id;
	facts = ZEND_MIR_CORE_ITEMS(module, value_facts, zend_mir_value_fact_ref);
	facts[module->value_facts.count] = fact;
	module->value_facts.count++;
	*out = id;
	return true;
}
