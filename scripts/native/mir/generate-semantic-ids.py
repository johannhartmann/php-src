#!/usr/bin/env python3
"""Generate the deterministic W01-to-ZNMIR semantic catalog binding."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MODEL = REPO_ROOT / "docs/native-engine/semantics/effects/effect-model.json"
DEFAULT_HEADER = REPO_ROOT / "Zend/Native/MIR/zend_mir_effects.h"
DEFAULT_OUTPUT = REPO_ROOT / "Zend/Native/MIR/Semantics/zend_mir_semantic_catalog.inc"

CATALOGS = {
    "effects": ("ZEND_MIR_EFFECT_CATALOG", "atomic_effects"),
    "memory_domains": ("ZEND_MIR_MEMORY_DOMAIN_CATALOG", "memory_domains"),
    "ownership_states": ("ZEND_MIR_OWNERSHIP_STATE_CATALOG", "ownership_states"),
    "ownership_actions": ("ZEND_MIR_OWNERSHIP_ACTION_CATALOG", "ownership_actions"),
    "predicates": ("ZEND_MIR_PREDICATE_CATALOG", "predicates"),
    "barriers": ("ZEND_MIR_BARRIER_CATALOG", "barriers"),
    "guard_facts": ("ZEND_MIR_GUARD_FACT_CATALOG", "guard_facts"),
    "composition_rules": ("ZEND_MIR_COMPOSITION_RULE_CATALOG", "composition_rules"),
}

ALIAS_KINDS = {
    "may_alias": "ZEND_MIR_ALIAS_MAY_ALIAS",
    "contains_reference": "ZEND_MIR_ALIAS_CONTAINS_REFERENCE",
    "indirect_access": "ZEND_MIR_ALIAS_INDIRECT_ACCESS",
    "shares_pointee": "ZEND_MIR_ALIAS_SHARES_POINTEE",
}

NORMAL_RETURN = {
    "permitted": "ZEND_MIR_NORMAL_RETURN_PERMITTED",
    "forbidden": "ZEND_MIR_NORMAL_RETURN_FORBIDDEN",
    "unchanged": "ZEND_MIR_NORMAL_RETURN_UNCHANGED",
}


class GenerationError(ValueError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GenerationError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise GenerationError(f"{path}: top level must be an object")
    return value


def parse_header_catalog(header: str, macro: str) -> list[tuple[str, int]]:
    marker = f"#define {macro}(X)"
    start = header.find(marker)
    if start < 0:
        raise GenerationError(f"frozen header lacks {macro}")
    end = header.find("\n\n", start)
    if end < 0:
        raise GenerationError(f"cannot find end of {macro}")
    block = header[start:end]
    entries = [
        (match.group(2), int(match.group(3)))
        for match in re.finditer(r'X\(([A-Z0-9_]+), "([^"]+)", ([0-9]+)\)', block)
    ]
    if not entries:
        raise GenerationError(f"frozen header has an empty {macro}")
    return entries


def require_id_list(value: Any, path: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise GenerationError(f"{path} must be an array of strings")
    if len(value) != len(set(value)):
        raise GenerationError(f"{path} contains duplicate IDs")
    return value


def index_records(model: dict[str, Any], key: str, expected: list[str]) -> dict[str, dict[str, Any]]:
    value = model.get(key)
    if not isinstance(value, list):
        raise GenerationError(f"$.{key} must be an array")
    records: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(value):
        if not isinstance(item, dict) or not isinstance(item.get("id"), str):
            raise GenerationError(f"$.{key}[{index}] must have a string ID")
        item_id = item["id"]
        if item_id in records:
            raise GenerationError(f"$.{key} duplicates {item_id}")
        records[item_id] = item
    if list(records) != expected:
        raise GenerationError(f"$.{key} order or membership differs from $.catalog")
    return records


def validate_model(model: dict[str, Any], header_text: str) -> dict[str, list[str]]:
    if model.get("format_version") != "1.0.0":
        raise GenerationError("effect model format_version must be 1.0.0")
    if model.get("model_id") != "php-native-effect-ownership-v1":
        raise GenerationError("unexpected effect model ID")
    catalog = model.get("catalog")
    if not isinstance(catalog, dict):
        raise GenerationError("$.catalog must be an object")

    ids: dict[str, list[str]] = {}
    for key, (macro, records_key) in CATALOGS.items():
        labels = require_id_list(catalog.get(key), f"$.catalog.{key}")
        frozen = parse_header_catalog(header_text, macro)
        if frozen != [(label, index) for index, label in enumerate(labels)]:
            raise GenerationError(f"$.catalog.{key} differs from frozen {macro}")
        index_records(model, records_key, labels)
        ids[key] = labels

    policy = model.get("unmodeled_semantics_policy")
    if not isinstance(policy, dict):
        raise GenerationError("$.unmodeled_semantics_policy must be an object")
    if (
        policy.get("classification") != "fail_closed"
        or policy.get("pure") is not False
        or policy.get("optimization") != "blocked"
        or policy.get("publication") != "rejected"
        or policy.get("ownership") != "rejected"
    ):
        raise GenerationError("unmodeled semantics policy is not fail-closed")
    for field, catalog_key in (
        ("effects", "effects"),
        ("reads", "memory_domains"),
        ("writes", "memory_domains"),
        ("barriers", "barriers"),
    ):
        if set(require_id_list(policy.get(field), f"$.unmodeled_semantics_policy.{field}")) != set(ids[catalog_key]):
            raise GenerationError(f"unmodeled policy {field} must contain the complete catalog")
    return ids


def bit_mask(names: Iterable[str], catalog: list[str], c_type: str, width: int) -> str:
    result = 0
    for name in names:
        try:
            result |= 1 << catalog.index(name)
        except ValueError as exc:
            raise GenerationError(f"unknown catalog ID {name}") from exc
    return f"{c_type}(0x{result:0{width}x})"


def state_mask(names: Iterable[str], catalog: list[str]) -> str:
    return bit_mask(names, catalog, "UINT8_C", 2)


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def macro(name: str, rows: list[str]) -> str:
    if not rows:
        raise GenerationError(f"cannot emit empty {name}")
    result = [f"#define {name}(X) \\"]
    for index, row in enumerate(rows):
        result.append(f"\tX({row})" + (" \\" if index + 1 < len(rows) else ""))
    return "\n".join(result)


def render(model_path: Path, header_path: Path) -> str:
    model_bytes = model_path.read_bytes()
    model = load_json(model_path)
    try:
        header_text = header_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GenerationError(f"cannot read {header_path}: {exc}") from exc
    ids = validate_model(model, header_text)

    effects = ids["effects"]
    domains = ids["memory_domains"]
    states = ids["ownership_states"]
    actions = ids["ownership_actions"]
    predicates = ids["predicates"]
    barriers = ids["barriers"]
    guards = ids["guard_facts"]
    rules = ids["composition_rules"]

    atomic_by_id = {item["id"]: item for item in model["atomic_effects"]}
    state_by_id = {item["id"]: item for item in model["ownership_states"]}
    action_by_id = {item["id"]: item for item in model["ownership_actions"]}
    predicate_by_id = {item["id"]: item for item in model["predicates"]}
    guard_by_id = {item["id"]: item for item in model["guard_facts"]}
    rule_by_id = {item["id"]: item for item in model["composition_rules"]}

    label_macros = []
    for macro_name, labels in (
        ("ZEND_MIR_GENERATED_EFFECT_LABELS", effects),
        ("ZEND_MIR_GENERATED_DOMAIN_LABELS", domains),
        ("ZEND_MIR_GENERATED_STATE_LABELS", states),
        ("ZEND_MIR_GENERATED_ACTION_LABELS", actions),
        ("ZEND_MIR_GENERATED_PREDICATE_LABELS", predicates),
        ("ZEND_MIR_GENERATED_BARRIER_LABELS", barriers),
        ("ZEND_MIR_GENERATED_GUARD_LABELS", guards),
        ("ZEND_MIR_GENERATED_RULE_LABELS", rules),
    ):
        label_macros.append(macro(macro_name, [f"{index}, {c_string(label)}" for index, label in enumerate(labels)]))

    atomic_rows = []
    for index, effect in enumerate(effects):
        item = atomic_by_id[effect]
        atomic_rows.append(
            ", ".join(
                (
                    str(index),
                    bit_mask(item["reads"], domains, "UINT32_C", 8),
                    bit_mask(item["writes"], domains, "UINT32_C", 8),
                    bit_mask(item["barriers"], barriers, "UINT8_C", 2),
                )
            )
        )

    state_rows = []
    for index, state in enumerate(states):
        item = state_by_id[state]
        cleanup = (
            "ZEND_MIR_CLEANUP_EXACTLY_ONE_RELEASE"
            if item["cleanup_obligations"] == "exactly_one_release"
            else "ZEND_MIR_CLEANUP_NONE"
        )
        state_rows.append(f"{index}, {'true' if item['terminal'] else 'false'}, {cleanup}")

    action_rows = []
    for index, action in enumerate(actions):
        item = action_by_id[action]
        unchanged = item["source_after"] == "unchanged"
        source_after = 0 if unchanged else states.index(item["source_after"])
        has_result = item["result_state"] is not None
        result_state = states.index(item["result_state"]) if has_result else 0
        action_rows.append(
            ", ".join(
                (
                    str(index),
                    state_mask(item["allowed_from"], states),
                    "true" if unchanged else "false",
                    str(source_after),
                    "true" if has_result else "false",
                    str(result_state),
                    bit_mask(item["effects"], effects, "UINT16_C", 4),
                    bit_mask(item["reads"], domains, "UINT32_C", 8),
                    bit_mask(item["writes"], domains, "UINT32_C", 8),
                    bit_mask(item["barriers"], barriers, "UINT8_C", 2),
                )
            )
        )

    alias_rows = []
    for item in model["alias_relations"]:
        alias_rows.append(
            f"{domains.index(item['left'])}, {domains.index(item['right'])}, {ALIAS_KINDS[item['kind']]}"
        )

    predicate_rows = []
    for index, predicate in enumerate(predicates):
        item = predicate_by_id[predicate]
        predicate_rows.append(f"{index}, {'true' if item['default_when_unproven'] else 'false'}")

    guard_rows = []
    for index, guard in enumerate(guards):
        item = guard_by_id[guard]
        invalid = item["invalidated_by"]
        guard_rows.append(
            ", ".join(
                (
                    str(index),
                    bit_mask(item["stable_across"], effects, "UINT16_C", 4),
                    bit_mask(invalid["effects"], effects, "UINT16_C", 4),
                    bit_mask(invalid["writes"], domains, "UINT32_C", 8),
                    bit_mask(invalid["barriers"], barriers, "UINT8_C", 2),
                    bit_mask(invalid["predicates"], predicates, "UINT8_C", 2),
                )
            )
        )

    rule_rows = []
    for index, rule in enumerate(rules):
        item = rule_by_id[rule]
        when = item["when"]
        implies = item["implies"]
        rule_rows.append(
            ", ".join(
                (
                    str(index),
                    bit_mask(when["predicates"], predicates, "UINT8_C", 2),
                    bit_mask(when["effects"], effects, "UINT16_C", 4),
                    bit_mask(when["actions"], actions, "UINT16_C", 4),
                    bit_mask(when["barriers"], barriers, "UINT8_C", 2),
                    bit_mask(implies["effects"], effects, "UINT16_C", 4),
                    bit_mask(implies["reads"], domains, "UINT32_C", 8),
                    bit_mask(implies["writes"], domains, "UINT32_C", 8),
                    bit_mask(implies["barriers"], barriers, "UINT8_C", 2),
                    NORMAL_RETURN[implies["normal_return"]],
                    "true" if rule == "unmodeled_internal_call" else "false",
                )
            )
        )

    policy = model["unmodeled_semantics_policy"]
    digest = hashlib.sha256(model_bytes).hexdigest()
    sections = [
        "/* Generated by scripts/native/mir/generate-semantic-ids.py. */",
        "/* Do not edit; regenerate from the frozen W01 effect model. */",
        f'#define ZEND_MIR_SEMANTIC_MODEL_SHA256 {c_string(digest)}',
        f"#define ZEND_MIR_GENERATED_ALIAS_RELATION_COUNT {len(alias_rows)}",
        "#define ZEND_MIR_GENERATED_FAIL_CLOSED_EFFECTS "
        + bit_mask(policy["effects"], effects, "UINT16_C", 4),
        "#define ZEND_MIR_GENERATED_FAIL_CLOSED_READS "
        + bit_mask(policy["reads"], domains, "UINT32_C", 8),
        "#define ZEND_MIR_GENERATED_FAIL_CLOSED_WRITES "
        + bit_mask(policy["writes"], domains, "UINT32_C", 8),
        "#define ZEND_MIR_GENERATED_FAIL_CLOSED_BARRIERS "
        + bit_mask(policy["barriers"], barriers, "UINT8_C", 2),
        *label_macros,
        macro("ZEND_MIR_GENERATED_ATOMIC_EFFECT_ROWS", atomic_rows),
        macro("ZEND_MIR_GENERATED_STATE_ROWS", state_rows),
        macro("ZEND_MIR_GENERATED_ACTION_ROWS", action_rows),
        macro("ZEND_MIR_GENERATED_ALIAS_ROWS", alias_rows),
        macro("ZEND_MIR_GENERATED_PREDICATE_ROWS", predicate_rows),
        macro("ZEND_MIR_GENERATED_GUARD_ROWS", guard_rows),
        macro("ZEND_MIR_GENERATED_RULE_ROWS", rule_rows),
    ]
    return "\n\n".join(sections) + "\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the generated output differs")
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        generated = render(args.model, args.header)
    except (OSError, GenerationError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"ERROR: cannot read generated output {args.output}: {exc}", file=sys.stderr)
            return 1
        if existing != generated:
            print(f"ERROR: generated semantic catalog is stale: {args.output}", file=sys.stderr)
            return 1
        print("ZNMIR semantic catalog matches the frozen W01 model")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8")
    print(f"wrote {args.output.relative_to(REPO_ROOT) if args.output.is_relative_to(REPO_ROOT) else args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
