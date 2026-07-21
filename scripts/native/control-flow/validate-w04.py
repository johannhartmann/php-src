#!/usr/bin/env python3
"""Validate W04 source wiring, profile decisions, and checked-in MIR goldens."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PROFILE = ROOT / "docs/native-engine/control-flow/w04-opcode-profile.json"
SOURCES = ROOT / "docs/native-engine/control-flow/w04-source-files.json"
CORPUS = ROOT / "tests/native/control-flow/corpus/manifest.json"
GOLDENS = ROOT / "tests/native/control-flow/integration/goldens/index.json"
CONFIG = ROOT / "ext/native_mir_test/config.m4"
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:TPDE|DynASM|zend_execute|execute_ex|zend_vm_call_opcode_handler|"
    r"mir_interpret|mir_evaluate)\b",
    re.IGNORECASE,
)


class ValidationError(RuntimeError):
    """A technical W04 invariant failed."""


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValidationError(f"{path.relative_to(ROOT)} must contain an object")
    return value


def config_sources() -> list[str]:
    text = CONFIG.read_text(encoding="utf-8")
    pattern = re.compile(
        r"PHP_ADD_SOURCES(?:_X)?\("
        r"\[(Zend/Native/(?:MIR|Lowering)/ControlFlow)\],\s*"
        r"\[([^\]]*)\]",
        re.MULTILINE,
    )
    sources: list[str] = []
    for match in pattern.finditer(text):
        sources.extend(
            f"{match.group(1)}/{name}"
            for name in re.findall(r"\b[a-zA-Z0-9_]+\.c\b", match.group(2))
        )
    if len(sources) != len(set(sources)):
        raise ValidationError("config.m4 lists a control-flow source twice")
    return sorted(sources)


def validate() -> None:
    profile = load(PROFILE)
    source_manifest = load(SOURCES)
    corpus = load(CORPUS)
    index = load(GOLDENS)

    expected_sources = sorted(source_manifest["w04_production_sources"])
    if config_sources() != expected_sources:
        raise ValidationError("config.m4 differs from w04-source-files.json")
    config = CONFIG.read_text(encoding="utf-8")
    if "PHP_ARG_ENABLE([native-mir-test]" not in config or "[no]," not in config:
        raise ValidationError("native_mir_test must remain default-off")
    for source in expected_sources:
        path = ROOT / source
        if not path.is_file():
            raise ValidationError(f"missing source: {source}")
        match = FORBIDDEN_RUNTIME.search(path.read_text(encoding="utf-8"))
        if match:
            raise ValidationError(f"runtime/target leak in {source}: {match.group(0)}")

    entries = profile.get("opcodes")
    if not isinstance(entries, list) or len(entries) != profile.get("active_opcode_count"):
        raise ValidationError("opcode profile count mismatch")
    numbers = [entry.get("number") for entry in entries]
    if numbers != sorted(numbers) or len(numbers) != len(set(numbers)):
        raise ValidationError("opcode profile numbers are not sorted and unique")
    if any(entry.get("deferred_wave") == "W04" for entry in entries):
        raise ValidationError("opcode profile retains a W04 deferral")
    by_name = {entry["opcode"]: entry for entry in entries}
    for opcode in ("ZEND_JMP", "ZEND_JMPZ", "ZEND_JMPNZ", "ZEND_JMPZ_EX", "ZEND_JMPNZ_EX"):
        if by_name[opcode]["classification"] == "deferred":
            raise ValidationError(f"{opcode} lost its source-backed decision")

    accepted = {
        case["case_id"]
        for case in corpus["cases"]
        if case["expected_status"] == "accepted"
    }
    golden_cases = index.get("cases")
    if index.get("format_version") != 1 or index.get("normalization") is not False:
        raise ValidationError("golden index envelope drift")
    if not isinstance(golden_cases, dict) or set(golden_cases) != accepted:
        raise ValidationError("goldens do not cover the accepted corpus exactly")
    totals: Counter[str] = Counter()
    for case_id, entry in golden_cases.items():
        path = ROOT / entry["path"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValidationError(f"golden hash mismatch: {case_id}")
        if not path.read_bytes().endswith(b"end\n"):
            raise ValidationError(f"incomplete MIR golden: {case_id}")
        totals.update(entry["cfg"])
    if totals["phis"] < 1 or totals["backedges"] < 1 or totals["statepoints"] < 1:
        raise ValidationError("goldens lack PHI, loop, or statepoint coverage")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    if not arguments.check:
        parser.error("only --check is supported")
    try:
        validate()
    except (ValidationError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"W04 integration validation failed: {error}", file=sys.stderr)
        return 1
    print("W04 integration validation passed: source wiring, profile, and MIR goldens")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
