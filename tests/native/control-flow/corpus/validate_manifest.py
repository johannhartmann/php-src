#!/usr/bin/env python3
"""Validate the schema, matrix, and immutable source identity of the W04 corpus."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
CORPUS = ROOT / "tests/native/control-flow/corpus"
MANIFEST = CORPUS / "manifest.json"
SCHEMA = CORPUS / "manifest.schema.json"

ACCEPTED_INTENTIONS = {
    "if_else_int",
    "nested_if_bool",
    "ternary_scalar",
    "short_circuit_and",
    "short_circuit_or",
    "early_return",
    "empty_fallthrough",
    "while_loop_counter",
    "do_while_counter",
    "for_loop_counter",
    "loop_carried_phi",
    "multiple_backedges_reducible",
}
REJECTED_INTENTIONS = {
    "try_catch_finally",
    "call_inside_branch",
    "reference_loop_phi",
    "array_condition",
    "object_condition",
    "string_condition_without_profile",
    "switch_or_match_if_not_profiled",
    "goto_irreducible",
    "unsupported_pi_constraint",
    "missing_condition_type_proof",
    "coalesce_reference_semantics",
    "nullsafe_jump",
}
REJECTED_CLASSIFICATIONS = {
    "try_catch_finally": (None, "MIRL0015"),
    "call_inside_branch": ("W05", "MIRL0012"),
    "reference_loop_phi": ("W06", "MIRL0013"),
    "array_condition": ("W06", "MIRL0013"),
    "object_condition": ("W06", "MIRL0013"),
    "string_condition_without_profile": ("W06", "MIRL0013"),
    "switch_or_match_if_not_profiled": ("W07", "MIRL0011"),
    "goto_irreducible": (None, "MIRL0016"),
    "unsupported_pi_constraint": (None, "MIRL0017"),
    "missing_condition_type_proof": ("W06", "MIRL0013"),
    "coalesce_reference_semantics": ("W06", "MIRL0013"),
    "nullsafe_jump": ("W06", "MIRL0013"),
}
FINAL_MIR_FIELDS = {
    "golden",
    "golden_path",
    "golden_sha256",
    "mir_hash",
    "mir_sha256",
    "expected_mir",
    "expected_mir_sha256",
}
CFG_KEYS = {
    "min_blocks",
    "min_edges",
    "min_phis",
    "requires_backedge",
    "requires_loop",
}


class ManifestError(ValueError):
    """The W04 evidence manifest is invalid."""


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError("{} is not valid JSON: {}".format(path, error)) from error


def json_type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return type(value) is int
    if expected == "boolean":
        return type(value) is bool
    if expected == "null":
        return value is None
    return False


def validate_schema_value(
    value: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    where: str = "$",
) -> None:
    if "$ref" in schema:
        reference = schema["$ref"]
        if not isinstance(reference, str) or not reference.startswith("#/"):
            raise ManifestError("{} uses an unsupported schema reference".format(where))
        target: Any = root_schema
        for token in reference[2:].split("/"):
            if not isinstance(target, dict) or token not in target:
                raise ManifestError("{} has an unresolved schema reference".format(where))
            target = target[token]
        validate_schema_value(value, target, root_schema, where)
        return
    expected_type = schema.get("type")
    if expected_type is not None:
        alternatives = expected_type if isinstance(expected_type, list) else [expected_type]
        if not any(json_type_matches(value, item) for item in alternatives):
            raise ManifestError(
                "{} must have schema type {}".format(where, expected_type)
            )
    if "const" in schema and value != schema["const"]:
        raise ManifestError("{} must equal {!r}".format(where, schema["const"]))
    if "enum" in schema and value not in schema["enum"]:
        raise ManifestError("{} is outside the schema enum".format(where))
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            raise ManifestError("{} is shorter than minLength".format(where))
        if "pattern" in schema and re.fullmatch(schema["pattern"], value) is None:
            raise ManifestError("{} does not match the schema pattern".format(where))
    if type(value) is int:
        if "minimum" in schema and value < schema["minimum"]:
            raise ManifestError("{} is below the schema minimum".format(where))
        if "maximum" in schema and value > schema["maximum"]:
            raise ManifestError("{} exceeds the schema maximum".format(where))
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            raise ManifestError("{} has too few items".format(where))
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            raise ManifestError("{} has too many items".format(where))
        if schema.get("uniqueItems") and len({json.dumps(item, sort_keys=True) for item in value}) != len(value):
            raise ManifestError("{} must contain unique items".format(where))
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                validate_schema_value(
                    item,
                    item_schema,
                    root_schema,
                    "{}[{}]".format(where, index),
                )
    if isinstance(value, dict):
        required = schema.get("required", [])
        missing = set(required) - value.keys()
        if missing:
            raise ManifestError(
                "{} is missing schema keys: {}".format(
                    where, ", ".join(sorted(missing))
                )
            )
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            unknown = value.keys() - properties.keys()
            if unknown:
                raise ManifestError(
                    "{} has unknown schema keys: {}".format(
                        where, ", ".join(sorted(unknown))
                    )
                )
        for key, child in value.items():
            if key in properties:
                validate_schema_value(
                    child,
                    properties[key],
                    root_schema,
                    "{}.{}".format(where, key),
                )


def source_function(source: str, function: str) -> tuple[str, str]:
    match = re.search(
        r"\bfunction\s+{}\s*\(".format(re.escape(function)),
        source,
    )
    if match is None:
        raise ManifestError("fixture does not define {}".format(function))
    open_brace = source.find("{", match.end())
    if open_brace < 0:
        raise ManifestError("{} has no function body".format(function))
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : open_brace], source[open_brace + 1 : index]
    raise ManifestError("{} has an unterminated function body".format(function))


def reject_final_mir_fields(value: Any, where: str = "$") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key in FINAL_MIR_FIELDS:
                raise ManifestError(
                    "{} contains integration-owned final MIR field {}".format(
                        where, key
                    )
                )
            reject_final_mir_fields(child, "{}.{}".format(where, key))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_final_mir_fields(child, "{}[{}]".format(where, index))


def validate_case(case: dict[str, Any]) -> None:
    case_id = case["case_id"]
    accepted = case["expected_status"] == "accepted"
    expected_directory = "accepted" if accepted else "rejected"
    if "/{}/".format(expected_directory) not in case["source_path"]:
        raise ManifestError("{} source directory disagrees with status".format(case_id))
    source_path = ROOT / case["source_path"]
    try:
        source_path.resolve().relative_to(CORPUS.resolve())
    except ValueError as error:
        raise ManifestError("{} source escapes the corpus".format(case_id)) from error
    if not source_path.is_file():
        raise ManifestError("{} source is missing".format(case_id))
    source_bytes = source_path.read_bytes()
    if hashlib.sha256(source_bytes).hexdigest() != case["source_sha256"]:
        raise ManifestError("{} source_sha256 does not match".format(case_id))
    try:
        source = source_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ManifestError("{} source must be UTF-8".format(case_id)) from error
    signature, body = source_function(source, case["function"])
    if case["repeat_calls"] != list(range(1, 11)):
        raise ManifestError("{} must attribute calls 1 through 10".format(case_id))
    if accepted:
        if case_id not in ACCEPTED_INTENTIONS:
            raise ManifestError("{} is not an accepted intention".format(case_id))
        if case["expected_mirl"] != "MIRL0000" or case["deferred_wave"] is not None:
            raise ManifestError("{} accepted classification is unstable".format(case_id))
        cfg = case["expected_cfg"]
        if not isinstance(cfg, dict) or set(cfg) != CFG_KEYS:
            raise ManifestError("{} must define the complete CFG minimums".format(case_id))
        for count in ("min_blocks", "min_edges", "min_phis"):
            if type(cfg[count]) is not int or cfg[count] < 0:
                raise ManifestError("{} has an invalid {}".format(case_id, count))
        for flag in ("requires_backedge", "requires_loop"):
            if type(cfg[flag]) is not bool:
                raise ManifestError("{} has an invalid {}".format(case_id, flag))
        if "&" in signature:
            raise ManifestError("{} accepted function uses references".format(case_id))
        forbidden = re.search(
            r"(?:\bnew\b|->|\?->|\bforeach\b|\byield\b|"
            r"\b(?:strlen|count|array_[a-z_]+)\s*\()",
            body,
        )
        if forbidden is not None:
            raise ManifestError(
                "{} accepted function contains a runtime/reference boundary".format(
                    case_id
                )
            )
    else:
        if case_id not in REJECTED_INTENTIONS:
            raise ManifestError("{} is not a rejected intention".format(case_id))
        if case["expected_cfg"] is not None:
            raise ManifestError("{} rejected case must not assert CFG minima".format(case_id))
        if (
            case["deferred_wave"],
            case["expected_mirl"],
        ) != REJECTED_CLASSIFICATIONS[case_id]:
            raise ManifestError("{} rejected classification drifted".format(case_id))


def validate_document(document: Any, schema: Any | None = None) -> None:
    if not isinstance(document, dict):
        raise ManifestError("manifest must be an object")
    schema_document = load_json(SCHEMA) if schema is None else schema
    if not isinstance(schema_document, dict):
        raise ManifestError("manifest schema must be an object")
    validate_schema_value(document, schema_document, schema_document)
    reject_final_mir_fields(document)
    cases = document["cases"]
    identifiers = [case["case_id"] for case in cases]
    if len(identifiers) != len(set(identifiers)):
        raise ManifestError("case IDs must be unique")
    if set(identifiers) != ACCEPTED_INTENTIONS | REJECTED_INTENTIONS:
        missing = (ACCEPTED_INTENTIONS | REJECTED_INTENTIONS) - set(identifiers)
        extra = set(identifiers) - (ACCEPTED_INTENTIONS | REJECTED_INTENTIONS)
        raise ManifestError(
            "corpus matrix mismatch; missing={} extra={}".format(
                sorted(missing), sorted(extra)
            )
        )
    for case in cases:
        validate_case(case)
    manifest_sources = {case["source_path"] for case in cases}
    fixture_sources = {
        path.relative_to(ROOT).as_posix()
        for directory in ("accepted", "rejected")
        for path in (CORPUS / directory).glob("*.php")
    }
    if manifest_sources != fixture_sources:
        raise ManifestError("manifest and fixture file sets differ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate checked-in files")
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required")
    try:
        schema = load_json(SCHEMA)
        document = load_json(MANIFEST)
        validate_document(document, schema)
    except ManifestError as error:
        print("W04 corpus validation failed: {}".format(error), file=sys.stderr)
        return 1
    accepted = sum(case["expected_status"] == "accepted" for case in document["cases"])
    rejected = len(document["cases"]) - accepted
    print(
        "W04 corpus manifest passed: {} accepted, {} rejected, "
        "schema valid, no final MIR goldens".format(accepted, rejected)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
