#!/usr/bin/env python3
"""Validate W05 source wiring, call-model decisions, and MIR goldens."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "docs/native-engine/calls/w05-opcode-profile.json"
SEQUENCE = ROOT / "docs/native-engine/calls/w05-sequence-profile.json"
RECLASSIFICATION = ROOT / "docs/native-engine/calls/w05-reclassification.json"
CORPUS = ROOT / "tests/native/calls/corpus/manifest.json"
GOLDENS = ROOT / "tests/native/calls/integration/goldens/index.json"
ARCHITECTURE_PATHS = (
    ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.c",
    ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.h",
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:zend_execute(?:_ex|_internal)?|execute_ex|zend_vm_execute|tpde|"
    r"dynam?asm|mir_interpret|machine_code_emit)\b",
    re.IGNORECASE,
)


class ValidationError(RuntimeError):
    """A technical W05 invariant failed."""


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValidationError(f"{path.relative_to(ROOT)} must contain an object")
    return value


def validate() -> None:
    profile = load(PROFILE)
    sequence = load(SEQUENCE)
    reclassification = load(RECLASSIFICATION)
    corpus = load(CORPUS)
    index = load(GOLDENS)

    completed = subprocess.run(
        [sys.executable, "scripts/native/build/update-native-test-sources.py", "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode:
        raise ValidationError(completed.stderr.strip() or completed.stdout.strip())

    entries = profile.get("opcodes")
    if not isinstance(entries, list) or len(entries) != profile.get("active_opcode_count"):
        raise ValidationError("opcode profile count mismatch")
    if profile.get("unresolved_w05_count") != 0:
        raise ValidationError("opcode profile retains an unresolved W05 decision")
    if not sequence.get("modeled") or sequence.get("codegen_eligible"):
        raise ValidationError("call model must remain model-only")
    if reclassification.get("unresolved_w05_count") != 0:
        raise ValidationError("reclassification retains an unresolved W05 decision")
    transitions = reclassification.get("reclassifications")
    if not isinstance(transitions, list) or len({item["opcode"] for item in transitions}) != len(transitions):
        raise ValidationError("reclassification entries are missing or duplicated")

    accepted = {case["id"] for case in corpus["cases"] if case["status"] == "accepted"}
    deferred = [case for case in corpus["cases"] if case["status"] != "accepted"]
    if any(not case.get("mirl") or not case.get("wave") for case in deferred):
        raise ValidationError("deferred corpus case lacks an exact diagnostic or destination")
    golden_cases = index.get("cases")
    if index.get("format_version") != "1.0.0" or index.get("normalization") is not False:
        raise ValidationError("golden index envelope drift")
    if not isinstance(golden_cases, dict) or set(golden_cases) != accepted:
        raise ValidationError("goldens do not cover the accepted corpus exactly")
    for case_id, entry in golden_cases.items():
        path = ROOT / entry["path"]
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValidationError(f"golden hash mismatch: {case_id}")
        if not data.endswith(b"end\n") or b"opcode call_direct_user" not in data:
            raise ValidationError(f"incomplete call-model golden: {case_id}")

    for path in ARCHITECTURE_PATHS:
        match = FORBIDDEN_RUNTIME.search(path.read_text(encoding="utf-8"))
        if match:
            raise ValidationError(
                f"runtime/target leak in {path.relative_to(ROOT)}: {match.group(0)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    if not arguments.check:
        parser.error("only --check is supported")
    try:
        validate()
    except (ValidationError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"W05 integration validation failed: {error}", file=sys.stderr)
        return 1
    print("W05 integration validation passed: source wiring, decisions, and MIR goldens")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
