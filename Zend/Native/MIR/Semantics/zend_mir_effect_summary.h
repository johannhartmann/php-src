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

#ifndef ZEND_MIR_EFFECT_SUMMARY_H
#define ZEND_MIR_EFFECT_SUMMARY_H

#include <stdbool.h>
#include <stdint.h>

#include "../zend_mir_effects.h"

typedef enum _zend_mir_normal_return_policy {
	ZEND_MIR_NORMAL_RETURN_UNCHANGED = 0,
	ZEND_MIR_NORMAL_RETURN_PERMITTED = 1,
	ZEND_MIR_NORMAL_RETURN_FORBIDDEN = 2
} zend_mir_normal_return_policy;

typedef struct _zend_mir_atomic_effect_descriptor {
	zend_mir_effect effect;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
} zend_mir_atomic_effect_descriptor;

typedef struct _zend_mir_composition_rule_descriptor {
	zend_mir_composition_rule rule;
	zend_mir_predicate_mask when_predicates;
	zend_mir_effect_mask when_effects;
	zend_mir_ownership_action_mask when_actions;
	zend_mir_barrier_mask when_barriers;
	zend_mir_effect_mask implied_effects;
	zend_mir_memory_domain_mask implied_reads;
	zend_mir_memory_domain_mask implied_writes;
	zend_mir_barrier_mask implied_barriers;
	zend_mir_normal_return_policy normal_return;
	bool rejects_publication;
} zend_mir_composition_rule_descriptor;

typedef struct _zend_mir_predicate_descriptor {
	zend_mir_predicate predicate;
	bool default_when_unproven;
} zend_mir_predicate_descriptor;

typedef struct _zend_mir_guard_descriptor {
	zend_mir_guard_fact fact;
	zend_mir_effect_mask stable_effects;
	zend_mir_effect_mask invalidating_effects;
	zend_mir_memory_domain_mask invalidating_writes;
	zend_mir_barrier_mask invalidating_barriers;
	zend_mir_predicate_mask invalidating_predicates;
} zend_mir_guard_descriptor;

typedef struct _zend_mir_effect_summary {
	zend_mir_effect_mask effects;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
	zend_mir_predicate_mask predicates;
	zend_mir_ownership_action_mask ownership_actions;
	zend_mir_composition_rule_mask applied_rules;
	bool normal_return;
	bool modeled;
} zend_mir_effect_summary;

#ifdef __cplusplus
extern "C" {
#endif

const char *zend_mir_semantic_model_sha256(void);
const char *zend_mir_effect_name(zend_mir_effect effect);
const char *zend_mir_memory_domain_name(zend_mir_memory_domain domain);
const char *zend_mir_barrier_name(zend_mir_barrier barrier);
const char *zend_mir_predicate_name(zend_mir_predicate predicate);
const char *zend_mir_guard_fact_name(zend_mir_guard_fact fact);
const char *zend_mir_composition_rule_name(zend_mir_composition_rule rule);

const zend_mir_atomic_effect_descriptor *zend_mir_atomic_effect_descriptor_at(
	zend_mir_effect effect);
const zend_mir_composition_rule_descriptor *zend_mir_composition_rule_descriptor_at(
	zend_mir_composition_rule rule);
const zend_mir_predicate_descriptor *zend_mir_predicate_descriptor_at(
	zend_mir_predicate predicate);
const zend_mir_guard_descriptor *zend_mir_guard_descriptor_at(zend_mir_guard_fact fact);

void zend_mir_effect_summary_empty(zend_mir_effect_summary *summary);
void zend_mir_effect_summary_fail_closed(zend_mir_effect_summary *summary);
bool zend_mir_effect_summary_init(zend_mir_effect_summary *summary,
	zend_mir_effect_mask effects, zend_mir_memory_domain_mask reads,
	zend_mir_memory_domain_mask writes, zend_mir_barrier_mask barriers,
	zend_mir_predicate_mask predicates, zend_mir_ownership_action_mask ownership_actions);
bool zend_mir_effect_summary_from_effect(
	zend_mir_effect effect, zend_mir_effect_summary *summary);
bool zend_mir_effect_summary_close(zend_mir_effect_summary *summary);
bool zend_mir_effect_summary_compose(zend_mir_effect_summary *result,
	const zend_mir_effect_summary *left, const zend_mir_effect_summary *right);
bool zend_mir_effect_summary_is_pure(const zend_mir_effect_summary *summary);
bool zend_mir_guard_fact_is_invalidated(
	zend_mir_guard_fact fact, const zend_mir_effect_summary *summary);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_EFFECT_SUMMARY_H */
