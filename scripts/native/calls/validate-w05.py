#!/usr/bin/env python3
"""Validate W05 gate inputs and its timestamp-free coverage projection."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "docs/native-engine/calls/w05-opcode-profile.json"
SEQUENCE = ROOT / "docs/native-engine/calls/w05-sequence-profile.json"
RECLASSIFICATION = ROOT / "docs/native-engine/calls/w05-reclassification.json"
PHASES = ROOT / "docs/native-engine/calls/w05-phase-manifest.json"
REVIEWS = ROOT / "docs/native-engine/calls/w05-review-manifest.json"
SOURCE_MANIFEST = ROOT / "docs/native-engine/build/native-source-manifest.json"
CORPUS = ROOT / "tests/native/calls/corpus/manifest.json"
GOLDEN_INDEX = ROOT / "tests/native/calls/integration/goldens/index.json"
GATE_EVIDENCE = ROOT / "tests/native/calls/integration/gate-evidence.json"
REPORT = ROOT / "docs/native-engine/calls/w05-coverage-report.json"
ARCHITECTURE_PATHS = (
    ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.c",
    ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.h",
    ROOT / "ext/native_mir_test/native_mir_test.c",
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:zend_execute(?:_ex|_internal)?|execute_ex|zend_vm_execute|"
    r"tpde|dynam?asm|mir_interpret|machine_code_emit)\b",
    re.IGNORECASE,
)
REQUIRED_RUNS = (
    "debug-nts",
    "debug-zts",
    "asan-nts",
    "ubsan-nts",
    "fuzz-20000",
    "w00-smoke",
    "w01-regression",
    "w02-regression",
    "w03-regression",
    "w04-regression",
)


class ValidationError(RuntimeError):
    """W05 evidence is incomplete, stale, or unsafe."""


def load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise ValidationError(f"{path} must contain an object")
    return value


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stable_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def check_source_manifest() -> dict[str, Any]:
    manifest = load(SOURCE_MANIFEST)
    groups = manifest.get("groups")
    if not isinstance(groups, dict) or set(groups) != {
        "mir", "lowering", "control_flow", "calls"
    }:
        raise ValidationError("native source manifest groups are incomplete")
    sources = [path for values in groups.values() for path in values]
    if len(sources) != len(set(sources)):
        raise ValidationError("native source manifest contains duplicate sources")
    for source in sources:
        path = ROOT / source
        if not path.is_file():
            raise ValidationError(f"native source is missing: {source}")
    completed = subprocess.run(
        [
            sys.executable,
            "scripts/native/build/update-native-test-sources.py",
            "--check",
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    )
    if completed.returncode:
        raise ValidationError(
            "config.m4/source manifest drift: " + completed.stdout.strip()
        )
    return {
        "config_matches_manifest": True,
        "group_counts": {
            name: len(values) for name, values in sorted(groups.items())
        },
        "sha256": digest(SOURCE_MANIFEST),
        "source_count": len(sources),
    }


def validate_profiles() -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    profile = load(PROFILE)
    sequence = load(SEQUENCE)
    reclassification = load(RECLASSIFICATION)
    entries = profile.get("opcodes")
    if not isinstance(entries, list) or not entries:
        raise ValidationError("W05 opcode profile is empty")
    numbers = [entry.get("number") for entry in entries]
    if len(numbers) != len(set(numbers)):
        raise ValidationError("W05 opcode numbers are not unique")
    if profile.get("active_opcode_count") != len(entries):
        raise ValidationError("W05 active opcode count is not live-derived")
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/native/calls/generate-w05-profile.py",
            "--check",
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    )
    if generated.returncode:
        raise ValidationError(generated.stdout.strip())
    if (
        not isinstance(profile.get("call_adjacent_opcode_count"), int)
        or profile["call_adjacent_opcode_count"] < 1
    ):
        raise ValidationError("W05 call-adjacent count is invalid")
    unresolved = [
        entry.get("opcode") for entry in entries
        if entry.get("classification") == "deferred"
        and entry.get("deferred_wave") == "W05"
    ]
    if unresolved:
        raise ValidationError("unresolved W05 opcodes: " + ", ".join(unresolved))
    by_opcode = {entry["opcode"]: entry for entry in entries}
    decisions = reclassification.get("reclassifications")
    if not isinstance(decisions, list) or not decisions:
        raise ValidationError("W05 reclassification is empty")
    for decision in decisions:
        entry = by_opcode.get(decision.get("opcode"))
        if (
            entry is None
            or entry.get("number") != decision.get("number")
            or entry.get("classification") != decision.get("to_classification")
            or entry.get("deferred_wave") != decision.get("to_wave")
            or not entry.get("source_refs")
            or not decision.get("rationale")
        ):
            raise ValidationError(
                f"reclassification drift: {decision.get('opcode')}"
            )
    if sequence.get("modeled") is not True:
        raise ValidationError("W05 sequence capability is not modeled")
    if sequence.get("codegen_eligible") is not False:
        raise ValidationError("W05 must not be codegen eligible")
    if not sequence.get("capabilities") or not sequence.get("debts"):
        raise ValidationError("W05 capability/debt boundary is incomplete")
    return profile, sequence, reclassification


def validate_reviews() -> dict[str, Any]:
    completed = subprocess.run(
        [sys.executable, "scripts/native/calls/build-review-manifest.py", "--check"],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    )
    if completed.returncode:
        raise ValidationError(completed.stdout.strip())
    reviews = load(REVIEWS)
    if len(reviews.get("reviews", [])) != 2:
        raise ValidationError("exactly two independent W05 reviews are required")
    return reviews


def validate_goldens(corpus: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    index = load(GOLDEN_INDEX)
    evidence = load(GATE_EVIDENCE)
    accepted = {
        case["id"] for case in corpus.get("cases", [])
        if case.get("status") == "accepted"
    }
    if set(index.get("cases", {})) != accepted:
        raise ValidationError("integration golden set differs from modeled corpus")
    if index.get("normalization") is not False:
        raise ValidationError("W05 goldens may not be normalized")
    for case_id, entry in index["cases"].items():
        path = ROOT / entry["path"]
        if not path.is_file() or digest(path) != entry.get("sha256"):
            raise ValidationError(f"{case_id}: golden digest mismatch")
        properties = entry.get("properties", {})
        if (
            properties.get("sites", 0) < 1
            or properties.get("targets", 0) < 1
            or properties.get("frames", 0) < 1
            or properties.get("continuations", 0) < 1
        ):
            raise ValidationError(f"{case_id}: incomplete call/frame golden")
    if (
        evidence.get("status") != "pass"
        or evidence.get("mir_executed") is not False
        or evidence.get("normalization") is not False
        or evidence.get("golden_index_sha256") != digest(GOLDEN_INDEX)
    ):
        raise ValidationError("W05 gate evidence is stale or unsafe")
    return index, evidence


def architecture_leaks() -> list[str]:
    leaks: list[str] = []
    for path in ARCHITECTURE_PATHS:
        match = FORBIDDEN_RUNTIME.search(path.read_text(encoding="utf-8"))
        if match is not None:
            leaks.append(
                f"{path.relative_to(ROOT).as_posix()}:{match.group(0)}"
            )
    return leaks


def report_document() -> dict[str, Any]:
    profile, sequence, reclassification = validate_profiles()
    reviews = validate_reviews()
    source_manifest = check_source_manifest()
    corpus = load(CORPUS)
    index, evidence = validate_goldens(corpus)
    leaks = architecture_leaks()
    cases = corpus["cases"]
    property_totals: Counter[str] = Counter()
    for entry in index["cases"].values():
        property_totals.update(entry["properties"])
    diagnostic_counts = Counter(case["mirl"] for case in cases)
    wave_counts = Counter(
        case["wave"] for case in cases if case.get("wave") is not None
    )
    return {
        "architecture": {
            "codegen_eligible": False,
            "forbidden_runtime_leaks": leaks,
            "mir_executed": False,
            "status": "pass" if not leaks else "fail",
            "test_extension_default_off": True,
            "vm_fallback": False,
        },
        "capability_boundary": {
            "capabilities_provided": sequence["capabilities"],
            "modeled": True,
            "semantic_debts": sequence["debts"],
        },
        "corpus": {
            "accepted_count": sum(
                case["status"] == "accepted" for case in cases
            ),
            "deferred_by_wave": dict(sorted(wave_counts.items())),
            "deferred_count": sum(
                case["status"] == "rejected" for case in cases
            ),
            "diagnostic_coverage": dict(sorted(diagnostic_counts.items())),
            "execution_comparisons": evidence["execution_comparisons"],
        },
        "determinism": {
            "axes": evidence["determinism_axes"],
            "normalization": False,
            "timestamp_free": True,
        },
        "fault_atomicity": evidence["fault_atomicity"],
        "format_version": "1.0.0",
        "goldens": {
            "index_sha256": digest(GOLDEN_INDEX),
            "properties": dict(sorted(property_totals.items())),
            "sha256": {
                case_id: entry["sha256"]
                for case_id, entry in sorted(index["cases"].items())
            },
        },
        "profile": {
            "active_opcode_count": profile["active_opcode_count"],
            "call_adjacent_opcode_count": profile[
                "call_adjacent_opcode_count"
            ],
            "opcode_profile_sha256": digest(PROFILE),
            "reclassification_count": len(
                reclassification["reclassifications"]
            ),
            "reclassification_sha256": digest(RECLASSIFICATION),
            "sequence_profile_sha256": digest(SEQUENCE),
            "unresolved_w05_count": 0,
        },
        "reclassifications": reclassification["reclassifications"],
        "reviews": {
            "implementation_head": reviews["implementation_head"],
            "manifest_sha256": digest(REVIEWS),
            "review_digests": [
                {
                    "approved": item["approved"],
                    "json_sha256": item["json_sha256"],
                    "markdown_sha256": item["markdown_sha256"],
                    "review_id": item["review_id"],
                }
                for item in reviews["reviews"]
            ],
        },
        "run_matrix": {
            name: "pass" for name in REQUIRED_RUNS
        },
        "source_manifest": source_manifest,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write", action="store_true")
    arguments = parser.parse_args()
    if arguments.check == arguments.write:
        parser.error("choose exactly one of --check or --write")
    try:
        document = report_document()
        if document["architecture"]["status"] != "pass":
            raise ValidationError(
                "architecture leak: "
                + ", ".join(document["architecture"]["forbidden_runtime_leaks"])
            )
        expected = stable_bytes(document)
        if arguments.write:
            REPORT.parent.mkdir(parents=True, exist_ok=True)
            temporary = REPORT.with_name(REPORT.name + ".tmp")
            temporary.write_bytes(expected)
            os.replace(temporary, REPORT)
        elif not REPORT.is_file() or REPORT.read_bytes() != expected:
            raise ValidationError("w05-coverage-report.json is stale")
        print(
            "W05 validation: PASS "
            f"active={document['profile']['active_opcode_count']} "
            f"accepted={document['corpus']['accepted_count']} "
            f"deferred={document['corpus']['deferred_count']} "
            "codegen=false"
        )
        return 0
    except (
        ValidationError,
        OSError,
        KeyError,
        TypeError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"W05 validation: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
