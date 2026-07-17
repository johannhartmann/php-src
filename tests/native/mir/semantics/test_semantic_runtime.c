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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "zend_mir_alias.h"
#include "zend_mir_effect_summary.h"
#include "zend_mir_ownership.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		return EXIT_FAILURE; \
	} \
} while (0)

static int test_effect_closure(void)
{
	zend_mir_effect_summary summary;
	zend_mir_effect_summary other;
	uint32_t index;

	zend_mir_effect_summary_empty(&summary);
	CHECK(zend_mir_effect_summary_is_pure(&summary));
	CHECK(!zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_INVALID, &summary));
	CHECK(!summary.modeled);
	CHECK(!zend_mir_effect_summary_is_pure(&summary));
	CHECK(summary.effects == UINT16_C(0x7fff));
	CHECK(summary.reads == UINT32_C(0x000fffff));
	CHECK(summary.writes == UINT32_C(0x000fffff));
	CHECK(summary.barriers == UINT8_C(0xff));
	CHECK(summary.predicates == UINT8_C(0x7f));
	CHECK(summary.ownership_actions == UINT16_C(0x03ff));
	CHECK(summary.applied_rules == UINT16_C(0x07ff));
	CHECK(!summary.normal_return);
	CHECK(!zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_CALL_INTERNAL, &summary));
	CHECK(!summary.modeled);

	CHECK(zend_mir_effect_summary_init(&summary, 0, 0, 0, 0,
		ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_RUN_DESTRUCTOR), 0));
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_RUN_DESTRUCTOR)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_REENTER_PHP)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_OBSERVE_FRAME)) != 0);
	CHECK((summary.barriers & ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR)) != 0);
	CHECK((summary.applied_rules
		& ZEND_MIR_COMPOSITION_RULE_MASK(
			ZEND_MIR_COMPOSITION_RULE_EXCEPTION_CLEANUP_ORDER)) != 0);

	CHECK(zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_CALL_PHP, &summary));
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_REENTER_PHP)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW)) != 0);
	CHECK((summary.barriers & ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_REENTRANCY)) != 0);

	CHECK(zend_mir_effect_summary_init(&summary, 0, 0, 0, 0,
		ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_INVOKE_MAGIC), 0));
	CHECK((summary.applied_rules
		& ZEND_MIR_COMPOSITION_RULE_MASK(
			ZEND_MIR_COMPOSITION_RULE_MAGIC_HANDLER_CLOSURE)) != 0);
	CHECK((summary.applied_rules
		& ZEND_MIR_COMPOSITION_RULE_MASK(
			ZEND_MIR_COMPOSITION_RULE_PHP_CALL_CLOSURE)) != 0);
	CHECK((summary.writes
		& ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_RUNTIME_CACHE)) != 0);

	CHECK(zend_mir_effect_summary_init(&summary, 0, 0, 0, 0,
		ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_OBSERVE_FRAME)
			| ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_INTERRUPT)
			| ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_SUSPEND), 0));
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_OBSERVE_FRAME)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_SUSPEND)) != 0);

	CHECK(zend_mir_effect_summary_init(&summary, 0, 0, 0, 0,
		ZEND_MIR_PREDICATE_MASK(ZEND_MIR_PREDICATE_MAY_BAILOUT), 0));
	CHECK(!summary.normal_return);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_BAILOUT)) != 0);

	CHECK(zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_READ_MEMORY, &summary));
	CHECK(zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_WRITE_MEMORY, &other));
	CHECK(zend_mir_effect_summary_compose(&summary, &summary, &other));
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_READ_MEMORY)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_WRITE_MEMORY)) != 0);
	CHECK(zend_mir_guard_fact_is_invalidated(ZEND_MIR_GUARD_FACT_VALUE_TYPE, &summary));

	CHECK(zend_mir_effect_summary_from_effect(ZEND_MIR_EFFECT_READ_MEMORY, &summary));
	CHECK(!zend_mir_guard_fact_is_invalidated(ZEND_MIR_GUARD_FACT_VALUE_TYPE, &summary));
	CHECK(zend_mir_guard_fact_is_invalidated(ZEND_MIR_GUARD_FACT_INVALID, &summary));

	CHECK(!zend_mir_effect_summary_init(&summary, UINT16_C(0x8000), 0, 0, 0, 0, 0));
	CHECK(!summary.modeled);
	CHECK(!zend_mir_effect_summary_init(&summary, 0, UINT32_C(0x00100000),
		0, 0, 0, 0));
	CHECK(!summary.modeled);
	CHECK(!zend_mir_effect_summary_init(&summary, 0, 0,
		UINT32_C(0x00100000), 0, 0, 0));
	CHECK(!summary.modeled);
	CHECK(!zend_mir_effect_summary_init(&summary, 0, 0, 0, 0,
		UINT8_C(0x80), 0));
	CHECK(!summary.modeled);
	CHECK(!zend_mir_effect_summary_init(&summary, 0, 0, 0, 0, 0,
		UINT16_C(0x0400)));
	CHECK(!summary.modeled);

	/*
	 * A caller-provided applied_rules bitmap must not suppress closure.
	 * close() recomputes the bitmap and restores every omitted implication.
	 */
	zend_mir_effect_summary_empty(&summary);
	summary.effects = ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_PHP);
	summary.applied_rules =
		ZEND_MIR_COMPOSITION_RULE_MASK(ZEND_MIR_COMPOSITION_RULE_PHP_CALL_CLOSURE);
	CHECK(zend_mir_effect_summary_close(&summary));
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_REENTER_PHP)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW)) != 0);
	CHECK((summary.effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_OBSERVE_FRAME)) != 0);
	CHECK((summary.writes
		& ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_RUNTIME_CACHE)) != 0);

	for (index = 0; index < ZEND_MIR_COMPOSITION_RULE_COUNT; index++) {
		const zend_mir_composition_rule_descriptor *rule =
			zend_mir_composition_rule_descriptor_at((zend_mir_composition_rule) index);
		bool accepted = zend_mir_effect_summary_init(&summary,
			rule->when_effects, 0, 0, rule->when_barriers,
			rule->when_predicates, rule->when_actions);
		CHECK(accepted != rule->rejects_publication);
		if (!accepted) {
			CHECK(!summary.modeled);
			CHECK(!zend_mir_effect_summary_is_pure(&summary));
			continue;
		}
		CHECK((summary.applied_rules & ZEND_MIR_COMPOSITION_RULE_MASK(index)) != 0);
		CHECK((summary.effects & rule->implied_effects) == rule->implied_effects);
		CHECK((summary.reads & rule->implied_reads) == rule->implied_reads);
		CHECK((summary.writes & rule->implied_writes) == rule->implied_writes);
		CHECK((summary.barriers & rule->implied_barriers) == rule->implied_barriers);
		if (rule->normal_return == ZEND_MIR_NORMAL_RETURN_FORBIDDEN) {
			CHECK(!summary.normal_return);
		}
	}
	return EXIT_SUCCESS;
}

static int test_alias_and_ownership(void)
{
	zend_mir_ownership_transition transition;
	zend_mir_ownership_state merged;
	zend_mir_cleanup_obligation cleanup;
	uint32_t state_index;
	uint32_t action_index;

	CHECK(zend_mir_alias_relation(ZEND_MIR_MEMORY_DOMAIN_FRAME_LOCALS,
		ZEND_MIR_MEMORY_DOMAIN_RUNTIME_SYMBOL_TABLE) == ZEND_MIR_ALIAS_INDIRECT_ACCESS);
	CHECK(zend_mir_alias_relation(ZEND_MIR_MEMORY_DOMAIN_HEAP_STRING,
		ZEND_MIR_MEMORY_DOMAIN_EXTERNAL_STATE) == ZEND_MIR_ALIAS_MAY_ALIAS);
	CHECK(zend_mir_alias_relation(ZEND_MIR_MEMORY_DOMAIN_INVALID,
		ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL) == ZEND_MIR_ALIAS_UNKNOWN);
	CHECK(zend_mir_domains_may_alias(ZEND_MIR_MEMORY_DOMAIN_INVALID,
		ZEND_MIR_MEMORY_DOMAIN_INVALID));
	CHECK(!zend_mir_domain_masks_may_alias(0,
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL)));
	CHECK(zend_mir_domain_masks_may_alias(UINT32_C(0x00100000),
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL)));
	CHECK(zend_mir_domain_masks_may_alias(
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_STRING),
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_EXTERNAL_STATE)));

	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_OWNED,
		ZEND_MIR_OWNERSHIP_ACTION_MOVE, &transition) == ZEND_MIR_OWNERSHIP_TRANSITION_OK);
	CHECK(transition.source_after == ZEND_MIR_OWNERSHIP_STATE_MOVED);
	CHECK(transition.has_result && transition.result_state == ZEND_MIR_OWNERSHIP_STATE_OWNED);
	CHECK(zend_mir_ownership_apply(transition.source_after,
		ZEND_MIR_OWNERSHIP_ACTION_MOVE, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE);
	CHECK(!transition.summary.modeled);

	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_OWNED,
		ZEND_MIR_OWNERSHIP_ACTION_DESTROY, &transition) == ZEND_MIR_OWNERSHIP_TRANSITION_OK);
	CHECK(transition.source_after == ZEND_MIR_OWNERSHIP_STATE_DESTROYED);
	CHECK(zend_mir_ownership_apply(transition.source_after,
		ZEND_MIR_OWNERSHIP_ACTION_DESTROY, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE);
	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_BORROWED,
		ZEND_MIR_OWNERSHIP_ACTION_DESTROY, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED);

	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_BORROWED,
		ZEND_MIR_OWNERSHIP_ACTION_CANONICALIZE, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_OK);
	CHECK(transition.source_after == ZEND_MIR_OWNERSHIP_STATE_BORROWED);
	CHECK(!transition.has_result);

	CHECK(zend_mir_ownership_phi_merge(ZEND_MIR_OWNERSHIP_STATE_OWNED,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, &merged, &cleanup) == ZEND_MIR_PHI_MERGE_ALLOWED);
	CHECK(merged == ZEND_MIR_OWNERSHIP_STATE_OWNED);
	CHECK(cleanup == ZEND_MIR_CLEANUP_EXACTLY_ONE_RELEASE);
	CHECK(zend_mir_ownership_phi_merge(ZEND_MIR_OWNERSHIP_STATE_BORROWED,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, &merged, &cleanup)
		== ZEND_MIR_PHI_MERGE_REQUIRES_CANONICALIZE);
	CHECK(merged == ZEND_MIR_OWNERSHIP_STATE_INVALID);
	CHECK(zend_mir_ownership_phi_merge(ZEND_MIR_OWNERSHIP_STATE_DESTROYED,
		ZEND_MIR_OWNERSHIP_STATE_DESTROYED, &merged, &cleanup)
		== ZEND_MIR_PHI_MERGE_REJECTED);

	for (state_index = 0; state_index < ZEND_MIR_OWNERSHIP_STATE_COUNT; state_index++) {
		const zend_mir_ownership_state_descriptor *state_descriptor =
			zend_mir_ownership_state_descriptor_at((zend_mir_ownership_state) state_index);
		uint32_t right_index;

		for (action_index = 0; action_index < ZEND_MIR_OWNERSHIP_ACTION_COUNT;
				action_index++) {
			const zend_mir_ownership_action_descriptor *action_descriptor =
				zend_mir_ownership_action_descriptor_at(
					(zend_mir_ownership_action) action_index);
			zend_mir_ownership_transition_status status = zend_mir_ownership_apply(
				(zend_mir_ownership_state) state_index,
				(zend_mir_ownership_action) action_index, &transition);

			if (state_descriptor->terminal) {
				CHECK(status == ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE);
			} else if ((action_descriptor->allowed_states & (UINT8_C(1) << state_index)) == 0) {
				CHECK(status == ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED);
			} else {
				CHECK(status == ZEND_MIR_OWNERSHIP_TRANSITION_OK);
				CHECK(transition.source_after == (action_descriptor->source_unchanged
					? (zend_mir_ownership_state) state_index : action_descriptor->source_after));
				CHECK(transition.has_result == action_descriptor->has_result);
				CHECK((transition.summary.effects & action_descriptor->effects)
					== action_descriptor->effects);
				CHECK((transition.summary.ownership_actions
					& ZEND_MIR_OWNERSHIP_ACTION_MASK(action_index)) != 0);
			}
		}

		for (right_index = 0; right_index < ZEND_MIR_OWNERSHIP_STATE_COUNT;
				right_index++) {
			bool left_usable = zend_mir_ownership_state_is_usable(
				(zend_mir_ownership_state) state_index);
			bool right_usable = zend_mir_ownership_state_is_usable(
				(zend_mir_ownership_state) right_index);
			zend_mir_phi_merge_status status = zend_mir_ownership_phi_merge(
				(zend_mir_ownership_state) state_index,
				(zend_mir_ownership_state) right_index, &merged, &cleanup);

			if (!left_usable || !right_usable) {
				CHECK(status == ZEND_MIR_PHI_MERGE_REJECTED);
			} else if (state_index != right_index) {
				CHECK(status == ZEND_MIR_PHI_MERGE_REQUIRES_CANONICALIZE);
			} else {
				CHECK(status == ZEND_MIR_PHI_MERGE_ALLOWED);
				CHECK((uint32_t) merged == state_index);
				CHECK(cleanup == state_descriptor->cleanup);
			}
		}
	}
	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_INVALID,
		ZEND_MIR_OWNERSHIP_ACTION_BORROW, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_STATE);
	CHECK(zend_mir_ownership_cleanup(ZEND_MIR_OWNERSHIP_STATE_INVALID)
		== ZEND_MIR_CLEANUP_INVALID);
	CHECK(zend_mir_ownership_apply(ZEND_MIR_OWNERSHIP_STATE_OWNED,
		ZEND_MIR_OWNERSHIP_ACTION_INVALID, &transition)
		== ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_ACTION);
	return EXIT_SUCCESS;
}

static void dump_catalog(void)
{
	uint32_t index;

	printf("M\t%s\n", zend_mir_semantic_model_sha256());
	for (index = 0; index < ZEND_MIR_EFFECT_COUNT; index++) {
		const zend_mir_atomic_effect_descriptor *item =
			zend_mir_atomic_effect_descriptor_at((zend_mir_effect) index);
		printf("E\t%" PRIu32 "\t%s\t%" PRIu16 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu8 "\n",
			index, zend_mir_effect_name((zend_mir_effect) index),
			ZEND_MIR_EFFECT_MASK(index), item->reads, item->writes, item->barriers);
	}
	for (index = 0; index < ZEND_MIR_MEMORY_DOMAIN_COUNT; index++) {
		printf("D\t%" PRIu32 "\t%s\n", index,
			zend_mir_memory_domain_name((zend_mir_memory_domain) index));
	}
	for (index = 0; index < ZEND_MIR_OWNERSHIP_STATE_COUNT; index++) {
		const zend_mir_ownership_state_descriptor *item =
			zend_mir_ownership_state_descriptor_at((zend_mir_ownership_state) index);
		printf("S\t%" PRIu32 "\t%s\t%d\t%d\n", index,
			zend_mir_ownership_state_name((zend_mir_ownership_state) index),
			item->terminal, item->cleanup);
	}
	for (index = 0; index < ZEND_MIR_OWNERSHIP_ACTION_COUNT; index++) {
		const zend_mir_ownership_action_descriptor *item =
			zend_mir_ownership_action_descriptor_at((zend_mir_ownership_action) index);
		printf("A\t%" PRIu32 "\t%s\t%" PRIu8 "\t%d\t%u\t%d\t%u\t%" PRIu16
			"\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu8 "\n",
			index, zend_mir_ownership_action_name((zend_mir_ownership_action) index),
			item->allowed_states, item->source_unchanged, (unsigned) item->source_after,
			item->has_result, (unsigned) item->result_state, item->effects,
			item->reads, item->writes, item->barriers);
	}
	for (index = 0; index < zend_mir_alias_descriptor_count(); index++) {
		const zend_mir_alias_descriptor *item = zend_mir_alias_descriptor_at(index);
		printf("L\t%" PRIu32 "\t%u\t%u\t%d\n", index,
			(unsigned) item->left, (unsigned) item->right, item->kind);
	}
	for (index = 0; index < ZEND_MIR_PREDICATE_COUNT; index++) {
		const zend_mir_predicate_descriptor *item =
			zend_mir_predicate_descriptor_at((zend_mir_predicate) index);
		printf("P\t%" PRIu32 "\t%s\t%d\n", index,
			zend_mir_predicate_name((zend_mir_predicate) index), item->default_when_unproven);
	}
	for (index = 0; index < ZEND_MIR_BARRIER_COUNT; index++) {
		printf("B\t%" PRIu32 "\t%s\n", index,
			zend_mir_barrier_name((zend_mir_barrier) index));
	}
	for (index = 0; index < ZEND_MIR_GUARD_FACT_COUNT; index++) {
		const zend_mir_guard_descriptor *item =
			zend_mir_guard_descriptor_at((zend_mir_guard_fact) index);
		printf("G\t%" PRIu32 "\t%s\t%" PRIu16 "\t%" PRIu16 "\t%" PRIu32
			"\t%" PRIu8 "\t%" PRIu8 "\n", index,
			zend_mir_guard_fact_name((zend_mir_guard_fact) index), item->stable_effects,
			item->invalidating_effects, item->invalidating_writes,
			item->invalidating_barriers, item->invalidating_predicates);
	}
	for (index = 0; index < ZEND_MIR_COMPOSITION_RULE_COUNT; index++) {
		const zend_mir_composition_rule_descriptor *item =
			zend_mir_composition_rule_descriptor_at((zend_mir_composition_rule) index);
		printf("R\t%" PRIu32 "\t%s\t%" PRIu8 "\t%" PRIu16 "\t%" PRIu16
			"\t%" PRIu8 "\t%" PRIu16 "\t%" PRIu32 "\t%" PRIu32
			"\t%" PRIu8 "\t%d\t%d\n", index,
			zend_mir_composition_rule_name((zend_mir_composition_rule) index),
			item->when_predicates, item->when_effects, item->when_actions,
			item->when_barriers, item->implied_effects, item->implied_reads,
			item->implied_writes, item->implied_barriers, item->normal_return,
			item->rejects_publication);
	}
}

int main(void)
{
	if (test_effect_closure() != EXIT_SUCCESS
			|| test_alias_and_ownership() != EXIT_SUCCESS) {
		return EXIT_FAILURE;
	}
	dump_catalog();
	return EXIT_SUCCESS;
}
