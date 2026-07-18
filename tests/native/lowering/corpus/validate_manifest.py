#!/usr/bin/env python3
"""Validate the deterministic W03 compile/dump corpus manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
CORPUS = ROOT / "tests/native/lowering/corpus"
DEFAULT_MANIFEST = CORPUS / "manifest.json"
SCHEMA = CORPUS / "manifest.schema.json"

ACCEPTED_FAMILIES = {
    "constants",
    "arithmetic",
    "bitwise",
    "comparisons",
    "booleans",
    "casts",
    "copy_move_return",
}
REJECTED_FAMILIES = {
    "w04_control_flow",
    "w05_runtime_effect",
    "w06_reference_semantics",
    "strings",
    "arrays",
    "objects",
    "references",
    "calls",
    "branches",
    "missing_proofs",
}
ALL_FAMILIES = ACCEPTED_FAMILIES | REJECTED_FAMILIES
REQUIRED_KEYS = {
    "case_id",
    "deferred_wave",
    "disposition",
    "expected_mirl",
    "family",
    "function",
    "golden_path",
    "golden_sha256",
    "reference_path",
    "repeat_calls",
    "source_path",
    "source_sha256",
}
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
CASE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
MIRL_PATTERN = re.compile(r"^MIRL[0-9]{4}$")
MIRL_CODES = {"MIRL{:04d}".format(code) for code in range(14)}


class ManifestError(ValueError):
    """The corpus manifest violates its checked-in schema or invariants."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError("{}: {}".format(path, error)) from error


def corpus_path(value: Any, where: str, prefixes: tuple[str, ...]) -> Path:
    if not isinstance(value, str) or not value:
        raise ManifestError("{} must be a non-empty relative path".format(where))
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ManifestError("{} must not escape the corpus".format(where))
    normalized = relative.as_posix()
    if not normalized.startswith(prefixes):
        raise ManifestError("{} is outside its owned corpus directory".format(where))
    path = ROOT / relative
    if not path.is_file():
        raise ManifestError("{} does not exist: {}".format(where, value))
    return path


def validate_environment(value: Any) -> None:
    expected = {
        "LANG": "C",
        "LC_ALL": "C",
        "SOURCE_DATE_EPOCH": "0",
        "TZ": "UTC",
        "random_seed": 0,
    }
    if value != expected:
        raise ManifestError("environment must exactly pin locale, timezone, epoch, and seed")


def validate_case(case: Any, index: int) -> None:
    where = "cases[{}]".format(index)
    if not isinstance(case, dict):
        raise ManifestError("{} must be an object".format(where))
    if set(case) != REQUIRED_KEYS:
        raise ManifestError(
            "{} keys differ from schema: missing={} unknown={}".format(
                where,
                sorted(REQUIRED_KEYS - case.keys()),
                sorted(case.keys() - REQUIRED_KEYS),
            )
        )
    if not isinstance(case["case_id"], str) or CASE_ID_PATTERN.fullmatch(case["case_id"]) is None:
        raise ManifestError("{}.case_id is invalid".format(where))
    if case["disposition"] not in {"accepted", "rejected"}:
        raise ManifestError("{}.disposition is invalid".format(where))
    if case["family"] not in ALL_FAMILIES:
        raise ManifestError("{}.family is invalid".format(where))
    if (
        not isinstance(case["expected_mirl"], str)
        or MIRL_PATTERN.fullmatch(case["expected_mirl"]) is None
        or case["expected_mirl"] not in MIRL_CODES
    ):
        raise ManifestError("{}.expected_mirl is invalid".format(where))
    if case["function"] is not None and (
        not isinstance(case["function"], str) or not case["function"]
    ):
        raise ManifestError("{}.function must be null or a non-empty string".format(where))
    calls = case["repeat_calls"]
    if (
        not isinstance(calls, list)
        or not calls
        or any(type(call) is not int or call < 1 or call > 10 for call in calls)
        or calls != sorted(set(calls))
    ):
        raise ManifestError("{}.repeat_calls must be sorted unique calls 1 through 10".format(where))
    source = corpus_path(
        case["source_path"],
        "{}.source_path".format(where),
        (
            "tests/native/lowering/corpus/accepted/",
            "tests/native/lowering/corpus/rejected/",
        ),
    )
    reference = corpus_path(
        case["reference_path"],
        "{}.reference_path".format(where),
        (
            "tests/native/lowering/corpus/accepted/",
            "tests/native/lowering/corpus/rejected/",
        ),
    )
    if source != reference:
        raise ManifestError(
            "{} must execute the exact source that is sent to the dump bridge".format(where)
        )
    if (
        not isinstance(case["source_sha256"], str)
        or SHA256_PATTERN.fullmatch(case["source_sha256"]) is None
        or sha256_file(source) != case["source_sha256"]
    ):
        raise ManifestError("{}.source_sha256 does not match source bytes".format(where))

    if case["disposition"] == "accepted":
        if case["family"] not in ACCEPTED_FAMILIES:
            raise ManifestError("{} accepted case uses a rejected family".format(where))
        if "/accepted/" not in case["source_path"]:
            raise ManifestError("{} accepted case must use an accepted fixture".format(where))
        if case["deferred_wave"] is not None or case["expected_mirl"] != "MIRL0000":
            raise ManifestError("{} accepted case has rejection metadata".format(where))
        golden = corpus_path(
            case["golden_path"],
            "{}.golden_path".format(where),
            ("tests/native/lowering/corpus/golden/",),
        )
        if (
            not isinstance(case["golden_sha256"], str)
            or SHA256_PATTERN.fullmatch(case["golden_sha256"]) is None
            or sha256_file(golden) != case["golden_sha256"]
        ):
            raise ManifestError("{}.golden_sha256 does not match golden bytes".format(where))
        if not golden.read_bytes().startswith(b"znmir "):
            raise ManifestError("{}.golden_path is not canonical ZNMIR text".format(where))
    else:
        if case["family"] not in REJECTED_FAMILIES:
            raise ManifestError("{} rejected case uses an accepted family".format(where))
        if "/rejected/" not in case["source_path"]:
            raise ManifestError("{} rejected case must use a rejected fixture".format(where))
        if case["golden_path"] is not None or case["golden_sha256"] is not None:
            raise ManifestError("{} rejected case must not publish partial MIR".format(where))
        if case["expected_mirl"] == "MIRL0000":
            raise ManifestError("{} rejected case expects MIRL0000".format(where))
        if case["deferred_wave"] not in {"W04", "W05", "W06", None}:
            raise ManifestError("{}.deferred_wave is invalid".format(where))


def validate_document(document: Any) -> None:
    if not isinstance(document, dict):
        raise ManifestError("manifest root must be an object")
    if set(document) != {"cases", "environment", "format_version", "manifest_id"}:
        raise ManifestError("manifest root keys differ from schema")
    if document["format_version"] != "1.0.0":
        raise ManifestError("format_version must be 1.0.0")
    if document["manifest_id"] != "w03-compile-dump-differential-v1":
        raise ManifestError("manifest_id is invalid")
    validate_environment(document["environment"])
    if not isinstance(document["cases"], list) or not document["cases"]:
        raise ManifestError("cases must be a non-empty array")
    case_ids = []
    source_paths = []
    for index, case in enumerate(document["cases"]):
        validate_case(case, index)
        case_ids.append(case["case_id"])
        source_paths.append(case["source_path"])
    if case_ids != sorted(case_ids):
        raise ManifestError("cases must be sorted by case_id")
    if len(case_ids) != len(set(case_ids)):
        raise ManifestError("case_id values must be unique")
    if len(source_paths) != len(set(source_paths)):
        raise ManifestError("source_path values must be unique")

    accepted = {
        case["family"] for case in document["cases"] if case["disposition"] == "accepted"
    }
    rejected = {
        case["family"] for case in document["cases"] if case["disposition"] == "rejected"
    }
    if accepted != ACCEPTED_FAMILIES:
        raise ManifestError(
            "accepted family coverage differs: missing={} extra={}".format(
                sorted(ACCEPTED_FAMILIES - accepted),
                sorted(accepted - ACCEPTED_FAMILIES),
            )
        )
    if rejected != REJECTED_FAMILIES:
        raise ManifestError(
            "rejected family coverage differs: missing={} extra={}".format(
                sorted(REJECTED_FAMILIES - rejected),
                sorted(rejected - REJECTED_FAMILIES),
            )
        )
    waves = {
        case["deferred_wave"]
        for case in document["cases"]
        if case["disposition"] == "rejected"
    }
    if not {"W04", "W05", "W06"}.issubset(waves):
        raise ManifestError("rejected corpus must cover W04, W05, and W06")
    repeated = [
        case for case in document["cases"] if case["repeat_calls"] == list(range(1, 11))
    ]
    if not repeated:
        raise ManifestError("at least one case must attribute calls 1 through 10")

    schema = load_json(SCHEMA)
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise ManifestError("manifest schema must use JSON Schema 2020-12")
    if schema.get("additionalProperties") is not False:
        raise ManifestError("manifest schema must reject unknown root properties")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--check", action="store_true", help="validate without writing")
    args = parser.parse_args()
    try:
        validate_document(load_json(args.manifest))
    except ManifestError as error:
        print("validate_manifest.py: {}".format(error), file=sys.stderr)
        return 1
    print("W03 lowering corpus manifest is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
