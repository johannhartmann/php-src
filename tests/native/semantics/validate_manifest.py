#!/usr/bin/env python3
"""Strictly validate the W01 semantic corpus manifest without dependencies."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Set


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST = Path(__file__).with_name("manifest.json")

FORMAT_VERSION = "1.0.0"
WAVE_BASE_COMMIT = "dc6e34b56846c38dc2475d6c962c2b9b7ada6df4"
PHP_SRC_SEMANTICS_COMMIT = "47355da494ba696b1bdb6d10448a225e742bd316"

REQUIRED_OPCODE_FAMILIES = {
    "arrays_cow",
    "assignments_temps",
    "calls_arguments_returns",
    "closures_dynamic_calls",
    "compare_branch_switch",
    "eval_include_require",
    "exceptions_finally",
    "fibers",
    "generators",
    "interrupt_ticks",
    "objects_properties_magic",
    "observer_user_opcode_hooks",
    "references_indirects",
    "scalars_type_juggling",
    "strings",
    "zts_relevant_paths",
}

REQUIRED_SEMANTIC_RISKS = {
    "array_cow",
    "bailout_nonlocal_transfer",
    "destructor_reentrancy",
    "destructor_throw",
    "dynamic_definition",
    "exception_finally_order",
    "fiber_suspend_resume",
    "generator_suspend_resume_close",
    "indirect_reference_aliasing",
    "interrupt_ticks",
    "named_by_ref_variadic_dynamic_calls",
    "observer_begin_end",
    "repeated_calls_1_to_10",
    "string_cow",
    "zts_configuration_path",
}

REQUIRED_BOUNDARIES = {
    "bailout_nonlocal_transfer": "bailout_nonlocal_transfer",
    "fiber_suspend_resume": "fiber_suspend_resume",
    "generator_suspend_resume_close": "generator_suspend_resume_close",
    "interrupt_ticks": "interrupt_tick_dispatch",
    "observer_begin_end": "observer_begin_end",
}

ALLOWED_KINDS = {"differential_php", "existing_phpt", "new_phpt"}
ALLOWED_MODES = {"cli", "nts", "phpt", "zend_test_observer", "zts"}
ALLOWED_CHANNELS = {
    "cleanup_order",
    "diagnostics",
    "exception_order",
    "exit_status",
    "observer_events",
    "stderr",
    "stdout",
    "suspension_state",
}
ALLOWED_DETERMINISM_CONSTRAINTS = {
    "byte_exact_output",
    "fixed_iteration_order",
    "fixed_locale_c",
    "fixed_timezone_utc",
    "no_external_io",
    "no_randomness",
    "no_wall_clock",
    "repository_local_inputs",
}

TOP_LEVEL_KEYS = {
    "fixtures",
    "format_version",
    "php_src_semantics_commit",
    "required_opcode_families",
    "required_semantic_risks",
    "wave_base_commit",
}
FIXTURE_KEYS = {
    "determinism_constraints",
    "fixture_id",
    "forced_boundaries",
    "kind",
    "observable_channels",
    "opcode_families",
    "opcodes",
    "owner",
    "reference_provenance_required",
    "repeat_calls",
    "required_extensions",
    "required_modes",
    "semantic_risks",
    "source_path",
}
BOUNDARY_KEYS = {"boundary", "mechanism", "source_anchors"}
IDENTIFIER_RE = re.compile(r"^[a-z][a-z0-9_]*$")
OPCODE_RE = re.compile(r"^ZEND_[A-Z0-9_]+$")
SOURCE_ANCHOR_RE = re.compile(r"^(?P<path>[^:]+):(?P<start>[1-9][0-9]*)(?:-(?P<end>[1-9][0-9]*))?$")
WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")
PLACEHOLDER_RE = re.compile(r"\b(?:TBD|TODO|later|probably|unknown)\b", re.IGNORECASE)


class ManifestError(ValueError):
    """A deterministic manifest contract violation."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def require_exact_keys(value: Mapping[str, Any], expected: Set[str], context: str) -> None:
    actual = set(value)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    require(not missing, "{} missing fields: {}".format(context, ", ".join(missing)))
    require(not extra, "{} has unknown fields: {}".format(context, ", ".join(extra)))


def require_identifier(value: Any, context: str) -> str:
    require(isinstance(value, str) and IDENTIFIER_RE.fullmatch(value) is not None,
            "{} must be a lower_snake_case identifier".format(context))
    return value


def require_sorted_unique_strings(value: Any, context: str, allow_empty: bool = True) -> List[str]:
    require(isinstance(value, list), "{} must be an array".format(context))
    require(allow_empty or bool(value), "{} must not be empty".format(context))
    require(all(isinstance(item, str) and item for item in value),
            "{} must contain non-empty strings".format(context))
    require(value == sorted(set(value)), "{} must be sorted and unique".format(context))
    return value


def require_relative_path(value: Any, context: str) -> str:
    require(isinstance(value, str) and value, "{} must be a non-empty path".format(context))
    require(not Path(value).is_absolute() and WINDOWS_ABSOLUTE_RE.match(value) is None,
            "{} must be repository-relative".format(context))
    require(".." not in Path(value).parts, "{} must not escape the repository".format(context))
    return value


def walk_strings(value: Any) -> Iterable[str]:
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from walk_strings(item)
    elif isinstance(value, dict):
        for item in value.values():
            yield from walk_strings(item)


def validate_boundary(boundary: Any, context: str, repository_root: Path) -> str:
    require(isinstance(boundary, dict), "{} must be an object".format(context))
    require_exact_keys(boundary, BOUNDARY_KEYS, context)
    boundary_id = require_identifier(boundary["boundary"], context + ".boundary")
    mechanism = boundary["mechanism"]
    require(isinstance(mechanism, str) and mechanism.strip(),
            "{}.mechanism must be non-empty".format(context))
    require(PLACEHOLDER_RE.search(mechanism) is None,
            "{}.mechanism contains an unresolved placeholder".format(context))
    anchors = require_sorted_unique_strings(boundary["source_anchors"], context + ".source_anchors", False)
    for anchor_index, anchor in enumerate(anchors):
        match = SOURCE_ANCHOR_RE.fullmatch(anchor)
        require(match is not None, "{}.source_anchors[{}] is malformed".format(context, anchor_index))
        assert match is not None
        relative = require_relative_path(match.group("path"), context + ".source_anchors path")
        source = repository_root / relative
        require(source.is_file(), "{} source anchor does not exist: {}".format(context, relative))
        with source.open("rb") as source_file:
            line_count = sum(1 for _ in source_file)
        start = int(match.group("start"))
        end = int(match.group("end") or start)
        require(start <= end <= line_count,
                "{} source anchor is outside {} ({} lines)".format(context, relative, line_count))
    return boundary_id


def validate_fixture(fixture: Any, index: int, repository_root: Path) -> Dict[str, Any]:
    context = "fixtures[{}]".format(index)
    require(isinstance(fixture, dict), "{} must be an object".format(context))
    require_exact_keys(fixture, FIXTURE_KEYS, context)
    fixture_id = require_identifier(fixture["fixture_id"], context + ".fixture_id")
    kind = fixture["kind"]
    require(kind in ALLOWED_KINDS, "{}.kind is not supported".format(context))
    source_path = require_relative_path(fixture["source_path"], context + ".source_path")
    source = repository_root / source_path
    require(source.is_file(), "{} source does not exist: {}".format(context, source_path))
    if kind == "existing_phpt":
        require(source_path.endswith(".phpt"), "{} existing PHPT must end in .phpt".format(context))
        require(source_path.startswith(("Zend/tests/", "ext/opcache/tests/", "ext/zend_test/tests/")),
                "{} existing PHPT is outside the declared discovery roots".format(context))
    elif kind == "new_phpt":
        require(source_path.startswith("tests/native/semantics/corpus/phpt/") and source_path.endswith(".phpt"),
                "{} new PHPT must be below corpus/phpt".format(context))
    else:
        require(source_path.startswith("tests/native/semantics/corpus/differential/") and source_path.endswith(".php"),
                "{} differential case must be below corpus/differential".format(context))

    families = require_sorted_unique_strings(fixture["opcode_families"], context + ".opcode_families", False)
    for family in families:
        require_identifier(family, context + ".opcode_families")
        require(family in REQUIRED_OPCODE_FAMILIES, "{} uses undeclared opcode family {}".format(context, family))
    opcodes = require_sorted_unique_strings(fixture["opcodes"], context + ".opcodes")
    require(all(OPCODE_RE.fullmatch(opcode) is not None for opcode in opcodes),
            "{}.opcodes must use ZEND_* names".format(context))
    risks = require_sorted_unique_strings(fixture["semantic_risks"], context + ".semantic_risks")
    for risk in risks:
        require_identifier(risk, context + ".semantic_risks")
        require(risk in REQUIRED_SEMANTIC_RISKS, "{} uses undeclared semantic risk {}".format(context, risk))
    extensions = require_sorted_unique_strings(fixture["required_extensions"], context + ".required_extensions")
    modes = require_sorted_unique_strings(fixture["required_modes"], context + ".required_modes", False)
    require(set(modes) <= ALLOWED_MODES, "{}.required_modes contains an unsupported mode".format(context))
    channels = require_sorted_unique_strings(fixture["observable_channels"], context + ".observable_channels", False)
    require(set(channels) <= ALLOWED_CHANNELS,
            "{}.observable_channels contains an unsupported channel".format(context))
    repeat_calls = fixture["repeat_calls"]
    require(isinstance(repeat_calls, list)
            and all(type(call) is int and 1 <= call <= 10 for call in repeat_calls),
            "{}.repeat_calls must contain integers 1 through 10".format(context))
    require(repeat_calls == sorted(set(repeat_calls)),
            "{}.repeat_calls must be sorted and unique".format(context))
    boundaries = fixture["forced_boundaries"]
    require(isinstance(boundaries, list), "{}.forced_boundaries must be an array".format(context))
    boundary_ids = [
        validate_boundary(boundary, "{}.forced_boundaries[{}]".format(context, boundary_index), repository_root)
        for boundary_index, boundary in enumerate(boundaries)
    ]
    require(boundary_ids == sorted(set(boundary_ids)),
            "{}.forced_boundaries must be sorted and unique by boundary".format(context))
    require(fixture["reference_provenance_required"] is True,
            "{} must require explicit reference provenance".format(context))
    constraints = require_sorted_unique_strings(
        fixture["determinism_constraints"], context + ".determinism_constraints", False)
    require(set(constraints) <= ALLOWED_DETERMINISM_CONSTRAINTS,
            "{}.determinism_constraints contains an unstable or unsupported assumption".format(context))
    require(fixture["owner"] == "W01-E", "{}.owner must be W01-E".format(context))
    if kind == "differential_php":
        require({"exit_status", "stderr", "stdout"} <= set(channels),
                "{} differential case must compare stdout, stderr, and exit status".format(context))
    if "observer_begin_end" in risks:
        require("zend_test" in extensions and "zend_test_observer" in modes,
                "{} observer fixture must require the zend_test observer".format(context))
    return fixture


def validate_document(document: Any, repository_root: Path = REPOSITORY_ROOT) -> Dict[str, Any]:
    require(isinstance(document, dict), "manifest must be an object")
    require_exact_keys(document, TOP_LEVEL_KEYS, "manifest")
    require(document["format_version"] == FORMAT_VERSION, "unsupported format_version")
    require(document["wave_base_commit"] == WAVE_BASE_COMMIT, "unexpected wave_base_commit")
    require(document["php_src_semantics_commit"] == PHP_SRC_SEMANTICS_COMMIT,
            "unexpected php_src_semantics_commit")
    families = require_sorted_unique_strings(document["required_opcode_families"],
                                             "required_opcode_families", False)
    require(set(families) == REQUIRED_OPCODE_FAMILIES,
            "required_opcode_families must equal the W01-E mandatory set")
    risks = require_sorted_unique_strings(document["required_semantic_risks"],
                                          "required_semantic_risks", False)
    require(set(risks) == REQUIRED_SEMANTIC_RISKS,
            "required_semantic_risks must equal the W01-E mandatory set")
    raw_fixtures = document["fixtures"]
    require(isinstance(raw_fixtures, list) and raw_fixtures, "fixtures must be a non-empty array")
    fixtures = [validate_fixture(fixture, index, repository_root) for index, fixture in enumerate(raw_fixtures)]
    fixture_ids = [fixture["fixture_id"] for fixture in fixtures]
    require(fixture_ids == sorted(fixture_ids), "fixtures must be sorted by fixture_id")
    require(len(fixture_ids) == len(set(fixture_ids)), "fixture_id values must be unique")

    covered_families = {family for fixture in fixtures for family in fixture["opcode_families"]}
    require(covered_families == REQUIRED_OPCODE_FAMILIES,
            "missing opcode family coverage: {}".format(", ".join(sorted(REQUIRED_OPCODE_FAMILIES - covered_families))))
    covered_risks = {risk for fixture in fixtures for risk in fixture["semantic_risks"]}
    require(covered_risks == REQUIRED_SEMANTIC_RISKS,
            "missing semantic risk coverage: {}".format(", ".join(sorted(REQUIRED_SEMANTIC_RISKS - covered_risks))))
    for risk, boundary_id in REQUIRED_BOUNDARIES.items():
        relevant = [fixture for fixture in fixtures if risk in fixture["semantic_risks"]]
        require(any(boundary_id in {item["boundary"] for item in fixture["forced_boundaries"]}
                    for fixture in relevant),
                "semantic risk {} lacks forced boundary {}".format(risk, boundary_id))

    attributed_calls = sorted({call for fixture in fixtures for call in fixture["repeat_calls"]})
    require(attributed_calls == list(range(1, 11)),
            "repeat calls 1 through 10 must be independently attributed")
    for value in walk_strings(document):
        require(not Path(value).is_absolute() and WINDOWS_ABSOLUTE_RE.match(value) is None,
                "manifest must not contain absolute paths: {}".format(value))
    return document


def load_and_validate(path: Path, repository_root: Path = REPOSITORY_ROOT) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as manifest_file:
        document = json.load(manifest_file)
    return validate_document(document, repository_root)


def main(argv: Sequence[str] = ()) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate and print a one-line summary")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--repository-root", type=Path, default=REPOSITORY_ROOT)
    args = parser.parse_args(argv or None)
    if not args.check:
        parser.error("--check is required")
    try:
        document = load_and_validate(args.manifest, args.repository_root.resolve())
    except (OSError, json.JSONDecodeError, ManifestError) as error:
        print("validate_manifest.py: {}".format(error), file=sys.stderr)
        return 1
    print("{}: valid ({} fixtures)".format(args.manifest, len(document["fixtures"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
