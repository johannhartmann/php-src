#!/usr/bin/env python3
"""Validate the native frame-state schema examples without third-party modules."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[3]
CONTRACT_DIR = REPO_ROOT / "docs/native-engine/semantics/frames"
DEFAULT_SCHEMA = CONTRACT_DIR / "frame-state.schema.json"
DEFAULT_EXAMPLES = CONTRACT_DIR / "frame-state.examples.json"

FRAME_REQUIRED = {
    "format_version",
    "frame_id",
    "function",
    "opline",
    "parent_frame_id",
    "slots",
    "roots",
    "cleanup_obligations",
    "continuations",
    "suspend",
    "code_version",
    "resume",
    "safepoint",
}
NESTED_REQUIRED = {
    "function": {"kind", "name", "op_array_id"},
    "opline": {"index", "phase", "owner_frame_id"},
    "suspend": {"kind", "state_id"},
    "code_version": {"id", "immutable", "active"},
    "resume": {
        "allowed",
        "entry_kind",
        "resume_id",
        "code_version_id",
        "target_opline_index",
        "machine_offset",
        "vm_dispatch",
    },
    "safepoint": {"class", "canonical"},
}
SLOT_REQUIRED = {
    "slot_id",
    "kind",
    "index",
    "representation",
    "materialization",
    "ownership",
    "rooted",
    "cleanup_required",
}
CLEANUP_REQUIRED = {"slot_id", "action", "state"}
CONTINUATION_REQUIRED = {"kind", "frame_id", "opline_index"}


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: top level must be an object")
    return value


def _missing_fields(value: Any, required: set[str]) -> bool:
    return not isinstance(value, dict) or bool(required - set(value))


def _matches_type(value: Any, expected: str) -> bool:
    checks = {
        "object": lambda item: isinstance(item, dict),
        "array": lambda item: isinstance(item, list),
        "string": lambda item: isinstance(item, str),
        "integer": lambda item: isinstance(item, int) and not isinstance(item, bool),
        "boolean": lambda item: isinstance(item, bool),
        "null": lambda item: item is None,
    }
    return checks[expected](value)


def schema_errors(
    value: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    path: str = "$",
) -> list[str]:
    """Evaluate the JSON Schema keywords used by frame-state.schema.json."""
    if "$ref" in schema:
        reference = schema["$ref"]
        if not reference.startswith("#/"):
            return [f"{path}: unsupported schema reference {reference}"]
        target: Any = root_schema
        for component in reference[2:].split("/"):
            target = target[component.replace("~1", "/").replace("~0", "~")]
        return schema_errors(value, target, root_schema, path)

    errors: list[str] = []
    expected_type = schema.get("type")
    if expected_type is not None:
        expected_types = expected_type if isinstance(expected_type, list) else [expected_type]
        if not any(_matches_type(value, item) for item in expected_types):
            return [f"{path}: expected type {expected_type}"]
    if "const" in schema and json.dumps(value, sort_keys=True) != json.dumps(
        schema["const"], sort_keys=True
    ):
        errors.append(f"{path}: expected constant {schema['const']!r}")
    if "enum" in schema and json.dumps(value, sort_keys=True) not in {
        json.dumps(item, sort_keys=True) for item in schema["enum"]
    }:
        errors.append(f"{path}: value is outside the enum")
    if isinstance(value, str) and len(value) < schema.get("minLength", 0):
        errors.append(f"{path}: string is too short")
    if isinstance(value, int) and not isinstance(value, bool) and "minimum" in schema:
        if value < schema["minimum"]:
            errors.append(f"{path}: number is below the minimum")

    if isinstance(value, dict):
        required = set(schema.get("required", []))
        for name in sorted(required - set(value)):
            errors.append(f"{path}: missing required property {name}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            for name in sorted(set(value) - set(properties)):
                errors.append(f"{path}: additional property {name}")
        for name, child_schema in properties.items():
            if name in value:
                errors.extend(schema_errors(value[name], child_schema, root_schema, f"{path}.{name}"))

    if isinstance(value, list):
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                errors.extend(schema_errors(item, item_schema, root_schema, f"{path}[{index}]"))
        if schema.get("uniqueItems") is True:
            encoded = [json.dumps(item, sort_keys=True) for item in value]
            if len(encoded) != len(set(encoded)):
                errors.append(f"{path}: array items are not unique")
    return errors


def validate_frame(frame: Any, schema: dict[str, Any] | None = None) -> set[str]:
    errors: set[str] = set()
    if schema is not None and schema_errors(frame, schema, schema):
        errors.add("schema-validation")
    if _missing_fields(frame, FRAME_REQUIRED):
        errors.add("missing-required-field")
        return errors

    for name, required in NESTED_REQUIRED.items():
        if _missing_fields(frame[name], required):
            errors.add("missing-required-field")

    slots = frame.get("slots")
    obligations = frame.get("cleanup_obligations")
    continuations = frame.get("continuations")
    if not isinstance(slots, list) or not isinstance(obligations, list):
        errors.add("missing-required-field")
        return errors
    if not isinstance(continuations, dict) or set(continuations) != {"return", "exception", "bailout"}:
        errors.add("missing-required-field")
    elif any(_missing_fields(item, CONTINUATION_REQUIRED) for item in continuations.values()):
        errors.add("missing-required-field")

    if any(_missing_fields(slot, SLOT_REQUIRED) for slot in slots):
        errors.add("missing-required-field")
    if any(_missing_fields(item, CLEANUP_REQUIRED) for item in obligations):
        errors.add("missing-required-field")

    slot_ids = [
        slot.get("slot_id")
        for slot in slots
        if isinstance(slot, dict) and isinstance(slot.get("slot_id"), str)
    ]
    if len(slot_ids) != len(set(slot_ids)):
        errors.add("duplicate-slot")

    roots = frame.get("roots")
    root_ids = {item for item in roots if isinstance(item, str)} if isinstance(roots, list) else set()
    known_ids = set(slot_ids)
    if not isinstance(roots, list) or not root_ids <= known_ids:
        errors.add("missing-root")
    for slot in slots:
        if isinstance(slot, dict) and slot.get("rooted") is True and slot.get("slot_id") not in root_ids:
            errors.add("missing-root")

    obligation_ids = {
        item.get("slot_id")
        for item in obligations
        if isinstance(item, dict) and item.get("state") in {"pending", "transferred"}
    }
    if not obligation_ids <= known_ids:
        errors.add("missing-cleanup")
    for slot in slots:
        if isinstance(slot, dict) and slot.get("cleanup_required") is True and slot.get("slot_id") not in obligation_ids:
            errors.add("missing-cleanup")

    code_version = frame.get("code_version", {})
    if not isinstance(code_version, dict):
        code_version = {}
    if code_version.get("immutable") is not True:
        errors.add("mutable-code-version")

    suspend = frame.get("suspend", {})
    if not isinstance(suspend, dict):
        suspend = {}
    if (suspend.get("kind") == "none") != (suspend.get("state_id") is None):
        errors.add("invalid-suspend-state")

    resume = frame.get("resume", {})
    if not isinstance(resume, dict):
        resume = {}
    if resume.get("allowed") is True:
        if (
            suspend.get("kind") == "none"
            or resume.get("entry_kind") != "single_entry_dispatcher"
            or not resume.get("resume_id")
            or resume.get("target_opline_index") is None
            or resume.get("machine_offset") is not None
            or resume.get("vm_dispatch") is not False
        ):
            errors.add("invalid-resume-target")
        if resume.get("code_version_id") != code_version.get("id"):
            errors.add("resume-version-mismatch")
    elif any(
        (
            resume.get("entry_kind") != "none",
            resume.get("resume_id") is not None,
            resume.get("code_version_id") is not None,
            resume.get("target_opline_index") is not None,
            resume.get("machine_offset") is not None,
            resume.get("vm_dispatch") is not False,
        )
    ):
        errors.add("invalid-resume-target")

    function = frame.get("function", {})
    opline = frame.get("opline", {})
    if not isinstance(function, dict):
        function = {}
    if not isinstance(opline, dict):
        opline = {}
    if function.get("kind") == "internal":
        if (
            function.get("op_array_id") is not None
            or opline.get("index") is not None
            or opline.get("owner_frame_id") != frame.get("parent_frame_id")
        ):
            errors.add("invalid-opline-owner")
    elif (
        function.get("op_array_id") is None
        or opline.get("index") is None
        or opline.get("owner_frame_id") != frame.get("frame_id")
    ):
        errors.add("invalid-opline-owner")
    return errors


def validate_example(example: Any, schema: dict[str, Any] | None = None) -> set[str]:
    if not isinstance(example, dict) or not isinstance(example.get("frames"), list):
        return {"missing-required-field"}

    errors: set[str] = set()
    frames = example["frames"]
    for frame in frames:
        errors.update(validate_frame(frame, schema))

    frame_ids = [
        frame.get("frame_id")
        for frame in frames
        if isinstance(frame, dict) and isinstance(frame.get("frame_id"), str)
    ]
    if len(frame_ids) != len(set(frame_ids)):
        errors.add("duplicate-frame")
    known_frames = set(frame_ids)
    parent_by_frame = {
        frame.get("frame_id"): frame.get("parent_frame_id")
        for frame in frames
        if isinstance(frame, dict) and frame.get("frame_id")
    }
    if any(parent is not None and parent not in known_frames for parent in parent_by_frame.values()):
        errors.add("missing-parent")

    for start in parent_by_frame:
        seen: set[str] = set()
        cursor: Any = start
        while cursor is not None and cursor in parent_by_frame:
            if cursor in seen:
                errors.add("parent-cycle")
                break
            seen.add(cursor)
            cursor = parent_by_frame[cursor]
    return errors


def validate_schema_contract(schema: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        problems.append("schema must use JSON Schema draft 2020-12")
    if set(schema.get("required", [])) != FRAME_REQUIRED:
        problems.append("schema top-level required fields do not match the frame contract")
    resume_required = (
        schema.get("properties", {}).get("resume", {}).get("required", [])
    )
    if set(resume_required) != NESTED_REQUIRED["resume"]:
        problems.append("schema resume fields do not match the resume contract")
    return problems


def check(schema_path: Path, examples_path: Path) -> int:
    schema = load_json(schema_path)
    document = load_json(examples_path)
    problems = validate_schema_contract(schema)

    examples = document.get("examples")
    if document.get("format_version") != "1.0.0" or not isinstance(examples, list):
        problems.append("examples document has an invalid envelope")
        examples = []

    seen_examples: set[str] = set()
    for example in examples:
        example_id = example.get("example_id") if isinstance(example, dict) else None
        if not example_id or example_id in seen_examples:
            problems.append("example IDs must be non-empty and unique")
            continue
        seen_examples.add(example_id)
        actual = sorted(validate_example(example, schema))
        expected = sorted(example.get("expected_errors", []))
        expected_valid = example.get("expected_valid")
        if expected_valid is not (not actual):
            problems.append(f"{example_id}: expected_valid disagrees with actual errors {actual}")
        if actual != expected:
            problems.append(f"{example_id}: expected {expected}, got {actual}")

    required_examples = {
        "normal-call",
        "exception-edge",
        "destructor-reentry",
        "generator-suspend",
        "fiber-suspend",
    }
    if not required_examples <= seen_examples:
        problems.append("examples document is missing a required valid scenario")

    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        return 1
    valid_count = sum(1 for example in examples if example["expected_valid"])
    invalid_count = len(examples) - valid_count
    print(f"frame contract OK: {valid_count} valid and {invalid_count} invalid examples")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check the schema and all examples")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--examples", type=Path, default=DEFAULT_EXAMPLES)
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required")
    return args


def main() -> int:
    args = parse_args()
    try:
        return check(args.schema, args.examples)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
