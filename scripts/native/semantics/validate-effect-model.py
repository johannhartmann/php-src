#!/usr/bin/env python3
"""Validate the native effect/alias/ownership contract without dependencies."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MODEL_PATH = ROOT / "docs/native-engine/semantics/effects/effect-model.json"
SCHEMA_PATH = ROOT / "docs/native-engine/semantics/effects/effect-model.schema.json"

FROZEN_EFFECTS = {
    "read_memory", "write_memory", "allocate", "throw", "bailout",
    "call_internal", "call_php", "reenter_php", "run_destructor",
    "observe_frame", "interrupt_boundary", "suspend", "resume",
    "external_io", "terminate",
}
FROZEN_DOMAINS = {
    "frame.args", "frame.locals", "frame.temps", "frame.call_chain",
    "runtime.symbol_table", "runtime.cache", "heap.zval", "heap.array",
    "heap.object", "heap.string", "heap.reference", "gc.metadata",
    "engine.exception", "engine.observer", "engine.interrupt",
    "engine.class_table", "engine.function_table", "engine.generator",
    "engine.fiber", "external.state",
}
FROZEN_ACTIONS = {
    "borrow", "copy_addref", "move", "produce_owned", "produce_borrowed",
    "transfer", "destroy", "conditional_destroy", "cow_separate",
    "canonicalize",
}
FROZEN_BARRIERS = {
    "safepoint", "reentrancy", "destructor", "exception", "bailout",
    "observer", "interrupt", "suspend",
}

NAMESPACE_ARRAYS = {
    "effects": "atomic_effects",
    "memory_domains": "memory_domains",
    "ownership_states": "ownership_states",
    "ownership_actions": "ownership_actions",
    "predicates": "predicates",
    "barriers": "barriers",
    "guard_facts": "guard_facts",
    "composition_rules": "composition_rules",
}


class ValidationError(ValueError):
    """A deterministic contract validation failure."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return json.load(handle, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot load {path}: {exc}") from exc


def _resolve_ref(schema: dict[str, Any], reference: str) -> Any:
    if not reference.startswith("#/"):
        raise ValidationError(f"unsupported schema reference: {reference}")
    node: Any = schema
    for raw in reference[2:].split("/"):
        key = raw.replace("~1", "/").replace("~0", "~")
        if not isinstance(node, dict) or key not in node:
            raise ValidationError(f"unresolved schema reference: {reference}")
        node = node[key]
    return node


def _is_type(value: Any, expected: str) -> bool:
    return {
        "object": lambda: isinstance(value, dict),
        "array": lambda: isinstance(value, list),
        "string": lambda: isinstance(value, str),
        "boolean": lambda: isinstance(value, bool),
        "integer": lambda: isinstance(value, int) and not isinstance(value, bool),
        "number": lambda: isinstance(value, (int, float)) and not isinstance(value, bool),
        "null": lambda: value is None,
    }[expected]()


def validate_schema_instance(
    value: Any, fragment: dict[str, Any], schema: dict[str, Any], path: str = "$"
) -> None:
    if "$ref" in fragment:
        validate_schema_instance(value, _resolve_ref(schema, fragment["$ref"]), schema, path)
    for part in fragment.get("allOf", []):
        validate_schema_instance(value, part, schema, path)
    if "const" in fragment and value != fragment["const"]:
        raise ValidationError(f"{path}: expected constant {fragment['const']!r}")
    if "enum" in fragment and value not in fragment["enum"]:
        raise ValidationError(f"{path}: value is not in enum")
    expected = fragment.get("type")
    if expected is not None:
        choices = expected if isinstance(expected, list) else [expected]
        if not any(_is_type(value, choice) for choice in choices):
            raise ValidationError(f"{path}: expected type {expected!r}")
    if isinstance(value, dict) and (fragment.get("type") == "object" or "properties" in fragment):
        properties = fragment.get("properties", {})
        for key in fragment.get("required", []):
            if key not in value:
                raise ValidationError(f"{path}: missing required property {key!r}")
        if fragment.get("additionalProperties") is False:
            extras = sorted(set(value) - set(properties))
            if extras:
                raise ValidationError(f"{path}: unexpected properties {extras!r}")
        for key, item in value.items():
            if key in properties:
                validate_schema_instance(item, properties[key], schema, f"{path}.{key}")
    if isinstance(value, list) and (fragment.get("type") == "array" or "items" in fragment):
        if len(value) < fragment.get("minItems", 0):
            raise ValidationError(f"{path}: too few items")
        if fragment.get("uniqueItems"):
            encoded = [json.dumps(item, sort_keys=True) for item in value]
            if len(encoded) != len(set(encoded)):
                raise ValidationError(f"{path}: duplicate array item")
        if "items" in fragment:
            for index, item in enumerate(value):
                validate_schema_instance(item, fragment["items"], schema, f"{path}[{index}]")
    if isinstance(value, str):
        if len(value) < fragment.get("minLength", 0):
            raise ValidationError(f"{path}: string is too short")
        if "pattern" in fragment and re.search(fragment["pattern"], value) is None:
            raise ValidationError(f"{path}: string does not match {fragment['pattern']!r}")


def _by_id(model: dict[str, Any], array_name: str) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in model[array_name]}


def _require_subset(actual: set[str], required: set[str], label: str) -> None:
    missing = sorted(required - actual)
    if missing:
        raise ValidationError(f"{label}: missing frozen IDs {missing}")


def _check_refs(values: list[str], valid: set[str], context: str) -> None:
    missing = sorted(set(values) - valid)
    if missing:
        raise ValidationError(f"{context}: unresolved IDs {missing}")


def _walk_strings(value: Any, path: str = "$") -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    if isinstance(value, str):
        found.append((path, value))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            found.extend(_walk_strings(item, f"{path}[{index}]"))
    elif isinstance(value, dict):
        for key, item in value.items():
            found.extend(_walk_strings(item, f"{path}.{key}"))
    return found


def _validate_sources(model: dict[str, Any]) -> None:
    refs: set[str] = set(model["source_basis"])
    for array_name in NAMESPACE_ARRAYS.values():
        for item in model[array_name]:
            refs.update(item.get("source_refs", []))
    for relation in model["alias_relations"]:
        refs.update(relation["source_refs"])
    pattern = re.compile(r"^(.+):(\d+)(?:-(\d+))?$")
    for reference in sorted(refs):
        match = pattern.fullmatch(reference)
        if not match:
            raise ValidationError(f"invalid source reference: {reference}")
        relative, first_text, last_text = match.groups()
        source = ROOT / relative
        if not source.is_file():
            raise ValidationError(f"source reference does not exist: {reference}")
        first = int(first_text)
        last = int(last_text or first_text)
        if first > last:
            raise ValidationError(f"reversed source range: {reference}")
        with source.open(encoding="utf-8", errors="replace") as handle:
            line_count = sum(1 for _ in handle)
        if last > line_count:
            raise ValidationError(f"source range exceeds file: {reference}")


def _validate_catalog(model: dict[str, Any]) -> dict[str, set[str]]:
    namespaces: dict[str, set[str]] = {}
    for catalog_name, array_name in NAMESPACE_ARRAYS.items():
        ids = [item["id"] for item in model[array_name]]
        if len(ids) != len(set(ids)):
            raise ValidationError(f"{array_name}: duplicate ID")
        catalog = model["catalog"][catalog_name]
        if catalog != ids:
            raise ValidationError(f"catalog.{catalog_name}: must exactly match {array_name} order")
        namespaces[catalog_name] = set(ids)
    return namespaces


def _validate_typed_references(model: dict[str, Any], ids: dict[str, set[str]]) -> None:
    effects, domains = ids["effects"], ids["memory_domains"]
    barriers, states = ids["barriers"], ids["ownership_states"]
    actions, predicates = ids["ownership_actions"], ids["predicates"]
    for effect in model["atomic_effects"]:
        _check_refs(effect["reads"], domains, f"effect {effect['id']} reads")
        _check_refs(effect["writes"], domains, f"effect {effect['id']} writes")
        _check_refs(effect["barriers"], barriers, f"effect {effect['id']} barriers")
    for domain in model["memory_domains"]:
        _check_refs(domain["materialized_at"], barriers, f"domain {domain['id']} materialized_at")
    for relation in model["alias_relations"]:
        _check_refs([relation["left"], relation["right"]], domains, "alias relation")
    for action in model["ownership_actions"]:
        _check_refs(action["allowed_from"], states, f"action {action['id']} allowed_from")
        if action["source_after"] != "unchanged":
            _check_refs([action["source_after"]], states, f"action {action['id']} source_after")
        if action["result_state"] is not None:
            _check_refs([action["result_state"]], states, f"action {action['id']} result_state")
        _check_refs(action["effects"], effects, f"action {action['id']} effects")
        _check_refs(action["reads"], domains, f"action {action['id']} reads")
        _check_refs(action["writes"], domains, f"action {action['id']} writes")
        _check_refs(action["barriers"], barriers, f"action {action['id']} barriers")
    for barrier in model["barriers"]:
        _check_refs(barrier["materialize"], domains, f"barrier {barrier['id']} materialize")
        _check_refs(barrier["forbids_motion_across"], effects, f"barrier {barrier['id']} effects")
    for guard in model["guard_facts"]:
        _check_refs(guard["stable_across"], effects, f"guard {guard['id']} stable_across")
        invalid = guard["invalidated_by"]
        _check_refs(invalid["effects"], effects, f"guard {guard['id']} invalidated effects")
        _check_refs(invalid["writes"], domains, f"guard {guard['id']} invalidated writes")
        _check_refs(invalid["barriers"], barriers, f"guard {guard['id']} invalidated barriers")
        _check_refs(invalid["predicates"], predicates, f"guard {guard['id']} invalidated predicates")
        overlap = sorted(set(guard["stable_across"]) & set(invalid["effects"]))
        if overlap:
            raise ValidationError(f"guard {guard['id']}: falsely stable across invalidating effects {overlap}")
    for rule in model["composition_rules"]:
        when, implies = rule["when"], rule["implies"]
        _check_refs(when["predicates"], predicates, f"rule {rule['id']} predicates")
        _check_refs(when["effects"], effects, f"rule {rule['id']} effects")
        _check_refs(when["actions"], actions, f"rule {rule['id']} actions")
        _check_refs(when["barriers"], barriers, f"rule {rule['id']} barriers")
        _check_refs(implies["effects"], effects, f"rule {rule['id']} implied effects")
        _check_refs(implies["reads"], domains, f"rule {rule['id']} reads")
        _check_refs(implies["writes"], domains, f"rule {rule['id']} writes")
        _check_refs(implies["barriers"], barriers, f"rule {rule['id']} implied barriers")


def _require_members(container: list[str], required: set[str], context: str) -> None:
    missing = sorted(required - set(container))
    if missing:
        raise ValidationError(f"{context}: missing required IDs {missing}")


def _validate_semantics(model: dict[str, Any], ids: dict[str, set[str]]) -> None:
    _require_subset(ids["effects"], FROZEN_EFFECTS, "effects")
    _require_subset(ids["memory_domains"], FROZEN_DOMAINS, "memory domains")
    _require_subset(ids["ownership_actions"], FROZEN_ACTIONS, "ownership actions")
    _require_subset(ids["barriers"], FROZEN_BARRIERS, "barriers")

    policy = model["unmodeled_semantics_policy"]
    if policy["pure"] or policy["optimization"] != "blocked" or policy["publication"] != "rejected" or policy["ownership"] != "rejected":
        raise ValidationError("unmodeled policy must fail closed and block optimization/publication")
    for key, namespace in (("effects", "effects"), ("reads", "memory_domains"), ("writes", "memory_domains"), ("barriers", "barriers")):
        if set(policy[key]) != ids[namespace]:
            raise ValidationError(f"unmodeled policy {key} must contain every registered ID")

    states = _by_id(model, "ownership_states")
    terminal = {name for name, state in states.items() if state["terminal"]}
    if terminal != {"moved", "released", "destroyed"}:
        raise ValidationError("ownership terminal states must be moved, released, and destroyed")
    for action in model["ownership_actions"]:
        illegal = sorted(set(action["allowed_from"]) & terminal)
        if illegal:
            raise ValidationError(f"action {action['id']}: terminal revival from {illegal}")
    action_map = _by_id(model, "ownership_actions")
    if action_map["move"]["allowed_from"] != ["owned"] or action_map["move"]["source_after"] != "moved":
        raise ValidationError("move must consume exactly one live owned source")
    if action_map["transfer"]["source_after"] != "moved":
        raise ValidationError("transfer must terminally consume its source")
    for name in ("destroy", "conditional_destroy"):
        action = action_map[name]
        _require_members(action["effects"], {"run_destructor", "reenter_php", "throw", "observe_frame"}, f"action {name}")
        _require_members(action["barriers"], {"safepoint", "reentrancy", "destructor", "exception", "observer"}, f"action {name}")
    if action_map["destroy"]["allowed_from"] != ["owned"]:
        raise ValidationError("destroy requires a uniquely owned live source")
    cow = action_map["cow_separate"]
    _require_members(cow["effects"], {"read_memory", "write_memory", "allocate"}, "cow_separate effects")
    _require_members(cow["writes"], {"heap.zval", "heap.array", "gc.metadata"}, "cow_separate writes")

    phi = model["phi_policy"]
    _check_refs(phi["allowed_inputs"], ids["ownership_states"], "phi allowed_inputs")
    if set(phi["allowed_inputs"]) & terminal:
        raise ValidationError("PHI policy permits terminal inputs")
    if set(phi["exit_kinds"]) != {"normal", "exception", "bailout", "suspend"}:
        raise ValidationError("PHI cleanup must cover every semantic exit kind")
    for merge in phi["implicit_merges"]:
        if len(set(merge)) != 1:
            raise ValidationError("PHI implicit merge combines different ownership states")

    rules = _by_id(model, "composition_rules")
    destructor = rules["destructor_closure"]["implies"]
    _require_members(destructor["effects"], {"run_destructor", "reenter_php", "throw", "observe_frame"}, "destructor closure effects")
    _require_members(destructor["barriers"], {"safepoint", "reentrancy", "destructor", "exception", "observer"}, "destructor closure barriers")
    bailout = rules["bailout_is_not_return"]["implies"]
    if bailout["normal_return"] != "forbidden" or "bailout" not in bailout["barriers"]:
        raise ValidationError("bailout rule must forbid normal return and install bailout barrier")
    internal = rules["unmodeled_internal_call"]["implies"]
    if set(internal["reads"]) != ids["memory_domains"] or set(internal["writes"]) != ids["memory_domains"] or set(internal["barriers"]) != ids["barriers"]:
        raise ValidationError("unmodeled internal call must use the complete fail-closed summary")

    barrier_map = _by_id(model, "barriers")
    visible_frame = {"frame.args", "frame.locals", "frame.temps", "frame.call_chain"}
    for name in ("safepoint", "reentrancy", "observer", "interrupt", "suspend"):
        _require_members(barrier_map[name]["materialize"], visible_frame, f"barrier {name} materialization")

    guards = _by_id(model, "guard_facts")
    fragile = {
        "refcount_unique": ({"write_memory", "call_internal", "call_php", "reenter_php", "run_destructor"}, {"heap.zval", "gc.metadata"}, {"reentrancy", "destructor"}),
        "array_packed_layout": ({"write_memory", "call_internal", "call_php", "reenter_php", "run_destructor"}, {"heap.array", "heap.reference"}, {"reentrancy", "destructor"}),
        "runtime_cache_entry": ({"write_memory", "call_internal", "call_php", "reenter_php"}, {"runtime.cache"}, {"reentrancy"}),
    }
    for name, (effects, writes, barriers) in fragile.items():
        invalid = guards[name]["invalidated_by"]
        _require_members(invalid["effects"], effects, f"guard {name} invalidating effects")
        _require_members(invalid["writes"], writes, f"guard {name} invalidating writes")
        _require_members(invalid["barriers"], barriers, f"guard {name} invalidating barriers")

    placeholder_words = "|".join(("T" + "BD", "TO" + "DO", "FIX" + "ME", "X" + "XX"))
    placeholder = re.compile(rf"\b(?:{placeholder_words})\b|(?:^|\s)<(?:fill|placeholder)[^>]*>", re.IGNORECASE)
    for path, text in _walk_strings(model):
        if placeholder.search(text):
            raise ValidationError(f"{path}: placeholder text is forbidden")


def validate_model(model: dict[str, Any], schema: dict[str, Any] | None = None) -> None:
    if schema is None:
        schema = load_json(SCHEMA_PATH)
    validate_schema_instance(model, schema, schema)
    ids = _validate_catalog(model)
    _validate_typed_references(model, ids)
    _validate_semantics(model, ids)
    _validate_sources(model)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate the canonical schema and model")
    args = parser.parse_args(argv)
    if not args.check:
        parser.error("--check is required")
    try:
        schema = load_json(SCHEMA_PATH)
        model = load_json(MODEL_PATH)
        validate_model(model, schema)
    except ValidationError as exc:
        print(f"effect model validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"effect model validation passed: {MODEL_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
