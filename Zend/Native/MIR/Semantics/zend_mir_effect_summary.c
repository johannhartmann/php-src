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

#include <stddef.h>

#include "zend_mir_alias.h"
#include "zend_mir_effect_summary.h"

#include "zend_mir_semantic_catalog.inc"

#define ZEND_MIR_ALL_PREDICATES ((zend_mir_predicate_mask) \
	((UINT16_C(1) << ZEND_MIR_PREDICATE_COUNT) - 1))
#define ZEND_MIR_ALL_ACTIONS ((zend_mir_ownership_action_mask) \
	((UINT16_C(1) << ZEND_MIR_OWNERSHIP_ACTION_COUNT) - 1))
#define ZEND_MIR_ALL_RULES ((zend_mir_composition_rule_mask) \
	((UINT16_C(1) << ZEND_MIR_COMPOSITION_RULE_COUNT) - 1))

static bool zend_mir_effect_summary_masks_are_valid(const zend_mir_effect_summary *summary)
{
	return (summary->effects & ~ZEND_MIR_GENERATED_FAIL_CLOSED_EFFECTS) == 0
		&& (summary->reads & ~ZEND_MIR_GENERATED_FAIL_CLOSED_READS) == 0
		&& (summary->writes & ~ZEND_MIR_GENERATED_FAIL_CLOSED_WRITES) == 0
		&& (summary->barriers & ~ZEND_MIR_GENERATED_FAIL_CLOSED_BARRIERS) == 0
		&& (summary->predicates & ~ZEND_MIR_ALL_PREDICATES) == 0
		&& (summary->ownership_actions & ~ZEND_MIR_ALL_ACTIONS) == 0
		&& (summary->applied_rules & ~ZEND_MIR_ALL_RULES) == 0;
}

void zend_mir_effect_summary_empty(zend_mir_effect_summary *summary)
{
	if (summary == NULL) {
		return;
	}
	summary->effects = 0;
	summary->reads = 0;
	summary->writes = 0;
	summary->barriers = 0;
	summary->predicates = 0;
	summary->ownership_actions = 0;
	summary->applied_rules = 0;
	summary->normal_return = true;
	summary->modeled = true;
}

void zend_mir_effect_summary_fail_closed(zend_mir_effect_summary *summary)
{
	if (summary == NULL) {
		return;
	}
	summary->effects = ZEND_MIR_GENERATED_FAIL_CLOSED_EFFECTS;
	summary->reads = ZEND_MIR_GENERATED_FAIL_CLOSED_READS;
	summary->writes = ZEND_MIR_GENERATED_FAIL_CLOSED_WRITES;
	summary->barriers = ZEND_MIR_GENERATED_FAIL_CLOSED_BARRIERS;
	summary->predicates = ZEND_MIR_ALL_PREDICATES;
	summary->ownership_actions = ZEND_MIR_ALL_ACTIONS;
	summary->applied_rules = ZEND_MIR_ALL_RULES;
	summary->normal_return = false;
	summary->modeled = false;
}

static bool zend_mir_composition_rule_matches(
	const zend_mir_composition_rule_descriptor *rule,
	const zend_mir_effect_summary *summary)
{
	bool has_condition = rule->when_predicates != 0 || rule->when_effects != 0
		|| rule->when_actions != 0 || rule->when_barriers != 0;

	return has_condition
		&& (summary->predicates & rule->when_predicates) == rule->when_predicates
		&& (summary->effects & rule->when_effects) == rule->when_effects
		&& (summary->ownership_actions & rule->when_actions) == rule->when_actions
		&& (summary->barriers & rule->when_barriers) == rule->when_barriers;
}

bool zend_mir_effect_summary_close(zend_mir_effect_summary *summary)
{
	uint32_t pass;

	if (summary == NULL) {
		return false;
	}
	if (!summary->modeled || !zend_mir_effect_summary_masks_are_valid(summary)) {
		zend_mir_effect_summary_fail_closed(summary);
		return false;
	}

	if ((summary->effects & (ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_BAILOUT)
			| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_TERMINATE))) != 0) {
		summary->normal_return = false;
	}

	for (pass = 0; pass <= ZEND_MIR_COMPOSITION_RULE_COUNT; pass++) {
		uint32_t index;
		bool changed = false;

		for (index = 0; index < ZEND_MIR_COMPOSITION_RULE_COUNT; index++) {
			const zend_mir_composition_rule_descriptor *rule;
			zend_mir_composition_rule_mask rule_bit =
				ZEND_MIR_COMPOSITION_RULE_MASK(index);

			if ((summary->applied_rules & rule_bit) != 0) {
				continue;
			}
			rule = zend_mir_composition_rule_descriptor_at(
				(zend_mir_composition_rule) index);
			if (!zend_mir_composition_rule_matches(rule, summary)) {
				continue;
			}
			if (rule->rejects_publication) {
				zend_mir_effect_summary_fail_closed(summary);
				return false;
			}
			summary->applied_rules |= rule_bit;
			summary->effects |= rule->implied_effects;
			summary->reads |= rule->implied_reads;
			summary->writes |= rule->implied_writes;
			summary->barriers |= rule->implied_barriers;
			if (rule->normal_return == ZEND_MIR_NORMAL_RETURN_FORBIDDEN) {
				summary->normal_return = false;
			} else if (rule->normal_return == ZEND_MIR_NORMAL_RETURN_PERMITTED) {
				summary->normal_return = true;
			}
			changed = true;
		}
		if (!changed) {
			return true;
		}
	}

	/* A finite rule set must reach a fixed point in at most one pass per rule. */
	zend_mir_effect_summary_fail_closed(summary);
	return false;
}

bool zend_mir_effect_summary_init(zend_mir_effect_summary *summary,
	zend_mir_effect_mask effects, zend_mir_memory_domain_mask reads,
	zend_mir_memory_domain_mask writes, zend_mir_barrier_mask barriers,
	zend_mir_predicate_mask predicates, zend_mir_ownership_action_mask ownership_actions)
{
	if (summary == NULL) {
		return false;
	}
	zend_mir_effect_summary_empty(summary);
	summary->effects = effects;
	summary->reads = reads;
	summary->writes = writes;
	summary->barriers = barriers;
	summary->predicates = predicates;
	summary->ownership_actions = ownership_actions;
	return zend_mir_effect_summary_close(summary);
}

bool zend_mir_effect_summary_from_effect(
	zend_mir_effect effect, zend_mir_effect_summary *summary)
{
	const zend_mir_atomic_effect_descriptor *descriptor;

	if (summary == NULL) {
		return false;
	}
	descriptor = zend_mir_atomic_effect_descriptor_at(effect);
	if (descriptor == NULL) {
		zend_mir_effect_summary_fail_closed(summary);
		return false;
	}
	return zend_mir_effect_summary_init(summary, ZEND_MIR_EFFECT_MASK(effect),
		descriptor->reads, descriptor->writes, descriptor->barriers, 0, 0);
}

bool zend_mir_effect_summary_compose(zend_mir_effect_summary *result,
	const zend_mir_effect_summary *left, const zend_mir_effect_summary *right)
{
	zend_mir_effect_summary left_copy;
	zend_mir_effect_summary right_copy;

	if (result == NULL) {
		return false;
	}
	if (left == NULL || right == NULL) {
		zend_mir_effect_summary_fail_closed(result);
		return false;
	}
	left_copy = *left;
	right_copy = *right;
	if (!left_copy.modeled || !right_copy.modeled
			|| !zend_mir_effect_summary_masks_are_valid(&left_copy)
			|| !zend_mir_effect_summary_masks_are_valid(&right_copy)) {
		zend_mir_effect_summary_fail_closed(result);
		return false;
	}
	result->effects = left_copy.effects | right_copy.effects;
	result->reads = left_copy.reads | right_copy.reads;
	result->writes = left_copy.writes | right_copy.writes;
	result->barriers = left_copy.barriers | right_copy.barriers;
	result->predicates = left_copy.predicates | right_copy.predicates;
	result->ownership_actions = left_copy.ownership_actions | right_copy.ownership_actions;
	result->applied_rules = left_copy.applied_rules | right_copy.applied_rules;
	result->normal_return = left_copy.normal_return && right_copy.normal_return;
	result->modeled = true;
	return zend_mir_effect_summary_close(result);
}

bool zend_mir_effect_summary_is_pure(const zend_mir_effect_summary *summary)
{
	return summary != NULL && summary->modeled && summary->normal_return
		&& summary->effects == 0 && summary->reads == 0 && summary->writes == 0
		&& summary->barriers == 0 && summary->predicates == 0
		&& summary->ownership_actions == 0;
}

bool zend_mir_guard_fact_is_invalidated(
	zend_mir_guard_fact fact, const zend_mir_effect_summary *summary)
{
	const zend_mir_guard_descriptor *guard = zend_mir_guard_descriptor_at(fact);

	if (guard == NULL || summary == NULL || !summary->modeled
			|| !zend_mir_effect_summary_masks_are_valid(summary)) {
		return true;
	}
	return (summary->effects & guard->invalidating_effects) != 0
		|| (summary->barriers & guard->invalidating_barriers) != 0
		|| (summary->predicates & guard->invalidating_predicates) != 0
		|| zend_mir_domain_masks_may_alias(summary->writes,
			guard->invalidating_writes);
}
