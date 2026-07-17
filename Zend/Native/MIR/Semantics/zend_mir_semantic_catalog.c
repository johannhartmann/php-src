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
#include "zend_mir_ownership.h"

#include "zend_mir_semantic_catalog.inc"

#define ZEND_MIR_LABEL_ROW(index, label) label,

static const char *const zend_mir_effect_names[] = {
	ZEND_MIR_GENERATED_EFFECT_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_domain_names[] = {
	ZEND_MIR_GENERATED_DOMAIN_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_state_names[] = {
	ZEND_MIR_GENERATED_STATE_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_action_names[] = {
	ZEND_MIR_GENERATED_ACTION_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_predicate_names[] = {
	ZEND_MIR_GENERATED_PREDICATE_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_barrier_names[] = {
	ZEND_MIR_GENERATED_BARRIER_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_guard_names[] = {
	ZEND_MIR_GENERATED_GUARD_LABELS(ZEND_MIR_LABEL_ROW)
};
static const char *const zend_mir_rule_names[] = {
	ZEND_MIR_GENERATED_RULE_LABELS(ZEND_MIR_LABEL_ROW)
};

#undef ZEND_MIR_LABEL_ROW

#define ZEND_MIR_ATOMIC_ROW(effect, reads, writes, barriers) \
	{(zend_mir_effect) (effect), (reads), (writes), (barriers)},
static const zend_mir_atomic_effect_descriptor zend_mir_atomic_effects[] = {
	ZEND_MIR_GENERATED_ATOMIC_EFFECT_ROWS(ZEND_MIR_ATOMIC_ROW)
};
#undef ZEND_MIR_ATOMIC_ROW

#define ZEND_MIR_STATE_ROW(state, terminal, cleanup) \
	{(zend_mir_ownership_state) (state), (terminal), (cleanup)},
static const zend_mir_ownership_state_descriptor zend_mir_states[] = {
	ZEND_MIR_GENERATED_STATE_ROWS(ZEND_MIR_STATE_ROW)
};
#undef ZEND_MIR_STATE_ROW

#define ZEND_MIR_ACTION_ROW(action, allowed, unchanged, source_after, has_result, result, effects, reads, writes, barriers) \
	{(zend_mir_ownership_action) (action), (allowed), (unchanged), \
		(zend_mir_ownership_state) (source_after), (has_result), \
		(zend_mir_ownership_state) (result), (effects), (reads), (writes), (barriers)},
static const zend_mir_ownership_action_descriptor zend_mir_actions[] = {
	ZEND_MIR_GENERATED_ACTION_ROWS(ZEND_MIR_ACTION_ROW)
};
#undef ZEND_MIR_ACTION_ROW

#define ZEND_MIR_ALIAS_ROW(left, right, kind) \
	{(zend_mir_memory_domain) (left), (zend_mir_memory_domain) (right), (kind)},
static const zend_mir_alias_descriptor zend_mir_aliases[] = {
	ZEND_MIR_GENERATED_ALIAS_ROWS(ZEND_MIR_ALIAS_ROW)
};
#undef ZEND_MIR_ALIAS_ROW

#define ZEND_MIR_PREDICATE_ROW(predicate, unproven) \
	{(zend_mir_predicate) (predicate), (unproven)},
static const zend_mir_predicate_descriptor zend_mir_predicates[] = {
	ZEND_MIR_GENERATED_PREDICATE_ROWS(ZEND_MIR_PREDICATE_ROW)
};
#undef ZEND_MIR_PREDICATE_ROW

#define ZEND_MIR_GUARD_ROW(fact, stable, effects, writes, barriers, predicates) \
	{(zend_mir_guard_fact) (fact), (stable), (effects), (writes), (barriers), (predicates)},
static const zend_mir_guard_descriptor zend_mir_guards[] = {
	ZEND_MIR_GENERATED_GUARD_ROWS(ZEND_MIR_GUARD_ROW)
};
#undef ZEND_MIR_GUARD_ROW

#define ZEND_MIR_RULE_ROW(rule, predicates, effects, actions, barriers, implied_effects, reads, writes, implied_barriers, normal_return, rejects) \
	{(zend_mir_composition_rule) (rule), (predicates), (effects), (actions), \
		(barriers), (implied_effects), (reads), (writes), (implied_barriers), \
		(normal_return), (rejects)},
static const zend_mir_composition_rule_descriptor zend_mir_rules[] = {
	ZEND_MIR_GENERATED_RULE_ROWS(ZEND_MIR_RULE_ROW)
};
#undef ZEND_MIR_RULE_ROW

#define ZEND_MIR_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_effect_names) == ZEND_MIR_EFFECT_COUNT,
	"generated effect labels cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_domain_names) == ZEND_MIR_MEMORY_DOMAIN_COUNT,
	"generated domain labels cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_state_names) == ZEND_MIR_OWNERSHIP_STATE_COUNT,
	"generated ownership states cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_action_names) == ZEND_MIR_OWNERSHIP_ACTION_COUNT,
	"generated ownership actions cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_predicate_names) == ZEND_MIR_PREDICATE_COUNT,
	"generated predicates cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_barrier_names) == ZEND_MIR_BARRIER_COUNT,
	"generated barriers cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_guard_names) == ZEND_MIR_GUARD_FACT_COUNT,
	"generated guard facts cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_rule_names) == ZEND_MIR_COMPOSITION_RULE_COUNT,
	"generated rules cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_atomic_effects) == ZEND_MIR_EFFECT_COUNT,
	"generated atomic effects cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_states) == ZEND_MIR_OWNERSHIP_STATE_COUNT,
	"generated state descriptors cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_actions) == ZEND_MIR_OWNERSHIP_ACTION_COUNT,
	"generated action descriptors cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_aliases)
		== ZEND_MIR_GENERATED_ALIAS_RELATION_COUNT,
	"generated aliases cover the frozen relations");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_predicates) == ZEND_MIR_PREDICATE_COUNT,
	"generated predicate descriptors cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_guards) == ZEND_MIR_GUARD_FACT_COUNT,
	"generated guard descriptors cover the frozen catalog");
ZEND_MIR_STATIC_ASSERT(ZEND_MIR_ARRAY_COUNT(zend_mir_rules) == ZEND_MIR_COMPOSITION_RULE_COUNT,
	"generated rule descriptors cover the frozen catalog");
#undef ZEND_MIR_ARRAY_COUNT

const char *zend_mir_semantic_model_sha256(void)
{
	return ZEND_MIR_SEMANTIC_MODEL_SHA256;
}

#define ZEND_MIR_NAME_FUNCTION(function_name, type, count, names) \
	const char *function_name(type value) \
	{ \
		return (uint32_t) value < (uint32_t) (count) ? (names)[(uint32_t) value] : NULL; \
	}

ZEND_MIR_NAME_FUNCTION(zend_mir_effect_name, zend_mir_effect,
	ZEND_MIR_EFFECT_COUNT, zend_mir_effect_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_memory_domain_name, zend_mir_memory_domain,
	ZEND_MIR_MEMORY_DOMAIN_COUNT, zend_mir_domain_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_ownership_state_name, zend_mir_ownership_state,
	ZEND_MIR_OWNERSHIP_STATE_COUNT, zend_mir_state_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_ownership_action_name, zend_mir_ownership_action,
	ZEND_MIR_OWNERSHIP_ACTION_COUNT, zend_mir_action_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_predicate_name, zend_mir_predicate,
	ZEND_MIR_PREDICATE_COUNT, zend_mir_predicate_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_barrier_name, zend_mir_barrier,
	ZEND_MIR_BARRIER_COUNT, zend_mir_barrier_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_guard_fact_name, zend_mir_guard_fact,
	ZEND_MIR_GUARD_FACT_COUNT, zend_mir_guard_names)
ZEND_MIR_NAME_FUNCTION(zend_mir_composition_rule_name, zend_mir_composition_rule,
	ZEND_MIR_COMPOSITION_RULE_COUNT, zend_mir_rule_names)

#undef ZEND_MIR_NAME_FUNCTION

const zend_mir_atomic_effect_descriptor *zend_mir_atomic_effect_descriptor_at(
	zend_mir_effect effect)
{
	return (uint32_t) effect < ZEND_MIR_EFFECT_COUNT
		? &zend_mir_atomic_effects[(uint32_t) effect] : NULL;
}

const zend_mir_composition_rule_descriptor *zend_mir_composition_rule_descriptor_at(
	zend_mir_composition_rule rule)
{
	return (uint32_t) rule < ZEND_MIR_COMPOSITION_RULE_COUNT
		? &zend_mir_rules[(uint32_t) rule] : NULL;
}

const zend_mir_predicate_descriptor *zend_mir_predicate_descriptor_at(
	zend_mir_predicate predicate)
{
	return (uint32_t) predicate < ZEND_MIR_PREDICATE_COUNT
		? &zend_mir_predicates[(uint32_t) predicate] : NULL;
}

const zend_mir_guard_descriptor *zend_mir_guard_descriptor_at(zend_mir_guard_fact fact)
{
	return (uint32_t) fact < ZEND_MIR_GUARD_FACT_COUNT
		? &zend_mir_guards[(uint32_t) fact] : NULL;
}

const zend_mir_ownership_state_descriptor *zend_mir_ownership_state_descriptor_at(
	zend_mir_ownership_state state)
{
	return (uint32_t) state < ZEND_MIR_OWNERSHIP_STATE_COUNT
		? &zend_mir_states[(uint32_t) state] : NULL;
}

const zend_mir_ownership_action_descriptor *zend_mir_ownership_action_descriptor_at(
	zend_mir_ownership_action action)
{
	return (uint32_t) action < ZEND_MIR_OWNERSHIP_ACTION_COUNT
		? &zend_mir_actions[(uint32_t) action] : NULL;
}

uint32_t zend_mir_alias_descriptor_count(void)
{
	return ZEND_MIR_GENERATED_ALIAS_RELATION_COUNT;
}

const zend_mir_alias_descriptor *zend_mir_alias_descriptor_at(uint32_t index)
{
	return index < ZEND_MIR_GENERATED_ALIAS_RELATION_COUNT
		? &zend_mir_aliases[index] : NULL;
}
