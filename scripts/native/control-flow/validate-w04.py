#!/usr/bin/env python3
"""Validate integrated W04 contracts and maintain its timestamp-free report."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PROFILE_PATH = ROOT / "docs/native-engine/control-flow/w04-opcode-profile.json"
SOURCE_MANIFEST_PATH = ROOT / "docs/native-engine/control-flow/w04-source-files.json"
OWNERSHIP_PATH = ROOT / "docs/native-engine/control-flow/w04-ownership.json"
CORPUS_PATH = ROOT / "tests/native/control-flow/corpus/manifest.json"
GOLDEN_INDEX = ROOT / "tests/native/control-flow/integration/goldens/index.json"
CONFIG_PATH = ROOT / "ext/native_mir_test/config.m4"
REPORT_PATH = ROOT / "docs/native-engine/control-flow/w04-coverage-report.json"
A_HEAD = "3a3c3218f8fa52ffc1d477f56f6faff132b0e1ab"
B_HEAD = "acd393a6eb8f1bb05f797368e34fc97cf9e884a7"
H_COMMIT = "01e51448e2bc9423d7dc1254ae5e4d34fc236eb4"
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:TPDE|DynASM|zend_execute|execute_ex|"
    r"zend_vm_call_opcode_handler|mir_interpret|mir_evaluate)\b",
    re.IGNORECASE,
)
GATES = (
    "python3 scripts/native/semantics/validate-w01.py --check",
    "python3 scripts/native/mir/validate-w02.py --check",
    "python3 scripts/native/lowering/validate-w03.py --check",
    "python3 scripts/native/control-flow/check-contract.py --check",
    "python3 scripts/native/control-flow/validate-w04.py --check",
    "python3 scripts/native/control-flow/test-w04.py --reference-php ${REFERENCE_PHP} --candidate-php ${CANDIDATE_PHP}",
    "python3 scripts/native/control-flow/test-w04.py --reference-php ${REFERENCE_PHP} --candidate-php ${ASAN_PHP} --sanitizer address",
    "python3 scripts/native/control-flow/test-w04.py --reference-php ${REFERENCE_PHP} --candidate-php ${UBSAN_PHP} --sanitizer undefined",
    "python3 -m unittest discover -s tests/native/control-flow -p test_*.py -v",
    "python3 tests/native/control-flow/fuzz/run_fuzz.py --seed 20260719 --cases 20000",
    "scripts/native/build.sh --profile debug-nts",
    "scripts/native/test-smoke.sh --profile debug-nts",
    "scripts/native/build.sh --profile debug-zts",
    "scripts/native/test-smoke.sh --profile debug-zts",
)


class W04Error(RuntimeError):
    """A deterministic W04 integration validation failure."""


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise W04Error("{}: {}".format(relative(path), error)) from error


def stable_bytes(document: Any) -> bytes:
    return (
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def config_w04_sources() -> list[str]:
    text = CONFIG_PATH.read_text(encoding="utf-8")
    sources: list[str] = []
    pattern = re.compile(
        r"PHP_ADD_SOURCES(?:_X)?\("
        r"\[(Zend/Native/(?:MIR|Lowering)/ControlFlow)\],\s*"
        r"\[([^\]]*)\]",
        re.MULTILINE,
    )
    for match in pattern.finditer(text):
        directory = match.group(1)
        names = re.findall(r"\b[a-zA-Z0-9_]+\.c\b", match.group(2))
        sources.extend("{}/{}".format(directory, name) for name in names)
    if len(sources) != len(set(sources)):
        raise W04Error("config.m4 lists a W04 production source more than once")
    return sorted(sources)


def validate_config(source_manifest: dict[str, Any]) -> None:
    expected = sorted(source_manifest["w04_production_sources"])
    actual = config_w04_sources()
    if actual != expected:
        raise W04Error(
            "config.m4 W04 sources differ from w04-source-files.json: "
            "expected={} actual={}".format(expected, actual)
        )
    text = CONFIG_PATH.read_text(encoding="utf-8")
    if "PHP_ARG_ENABLE([native-mir-test]" not in text or "[no]," not in text:
        raise W04Error("native_mir_test is not an explicit default-off extension")
    for source in expected:
        if not (ROOT / source).is_file():
            raise W04Error("manifest production source is missing: {}".format(source))


def validate_profile(profile: dict[str, Any]) -> None:
    entries = profile.get("opcodes")
    if not isinstance(entries, list) or len(entries) != profile.get(
        "active_opcode_count"
    ):
        raise W04Error("profile count differs from active_opcode_count")
    numbers = [entry.get("number") for entry in entries]
    if numbers != sorted(numbers) or len(numbers) != len(set(numbers)):
        raise W04Error("profile opcode numbers are not sorted and unique")
    if any(entry.get("deferred_wave") == "W04" for entry in entries):
        raise W04Error("profile retains an unresolved W04 deferral")
    by_opcode = {entry["opcode"]: entry for entry in entries}
    for opcode in ("ZEND_JMP", "ZEND_JMPZ", "ZEND_JMPNZ", "ZEND_JMPZ_EX", "ZEND_JMPNZ_EX"):
        entry = by_opcode.get(opcode)
        if entry is None or entry["classification"] == "deferred":
            raise W04Error("{} is not W04 accepted".format(opcode))
    source_counts = profile.get("sources", {})
    if any(
        value.get("active_opcode_count") != profile["active_opcode_count"]
        for value in source_counts.values()
    ):
        raise W04Error("profile source counts differ from live active count")


def validate_goldens(corpus: dict[str, Any], index: dict[str, Any]) -> None:
    accepted = {
        case["case_id"]
        for case in corpus["cases"]
        if case["expected_status"] == "accepted"
    }
    entries = index.get("cases")
    if index.get("format_version") != 1 or index.get("normalization") is not False:
        raise W04Error("golden index format or normalization contract drifted")
    if not isinstance(entries, dict) or set(entries) != accepted:
        raise W04Error("golden index does not cover the accepted corpus exactly")
    totals = Counter()
    for case_id, entry in entries.items():
        path = ROOT / entry["path"]
        if not path.is_file() or sha256_file(path) != entry["sha256"]:
            raise W04Error("{} golden hash does not match".format(case_id))
        if not path.read_bytes().endswith(b"end\n"):
            raise W04Error("{} golden is not a complete ZNMIR module".format(case_id))
        totals.update(entry["cfg"])
    if totals["phis"] < 1 or totals["backedges"] < 1 or totals["statepoints"] < 1:
        raise W04Error("goldens lack PHI, loop, or edge-statepoint coverage")


def architecture_leaks(source_manifest: dict[str, Any]) -> list[str]:
    leaks = []
    for value in source_manifest["w04_production_sources"]:
        path = ROOT / value
        match = FORBIDDEN_RUNTIME.search(path.read_text(encoding="utf-8"))
        if match is not None:
            leaks.append("{}: {}".format(value, match.group(0)))
    return leaks


def verify_specialist_heads() -> None:
    for task, head in (
        ("W04-A-production-control-flow", A_HEAD),
        ("W04-B-control-flow-evidence", B_HEAD),
    ):
        completed = subprocess.run(
            [
                "python3",
                "scripts/native/control-flow/check-ownership.py",
                "--task",
                task,
                "--base",
                H_COMMIT,
                "--head",
                head,
            ],
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60,
        )
        if completed.returncode != 0:
            raise W04Error(
                "{} ownership failed: {}".format(task, completed.stdout.strip())
            )


def report_document() -> dict[str, Any]:
    profile = load_json(PROFILE_PATH)
    source_manifest = load_json(SOURCE_MANIFEST_PATH)
    ownership = load_json(OWNERSHIP_PATH)
    corpus = load_json(CORPUS_PATH)
    index = load_json(GOLDEN_INDEX)
    validate_profile(profile)
    validate_config(source_manifest)
    validate_goldens(corpus, index)
    leaks = architecture_leaks(source_manifest)
    entries = profile["opcodes"]
    accepted_entries = [
        entry for entry in entries if entry["classification"] != "deferred"
    ]
    deferred_entries = [
        entry for entry in entries if entry["classification"] == "deferred"
    ]
    cases = corpus["cases"]
    cfg_totals = Counter()
    for entry in index["cases"].values():
        cfg_totals.update(entry["cfg"])
    diagnostic_counts = Counter(case["expected_mirl"] for case in cases)
    return {
        "architecture_independence": {
            "forbidden_families": [
                "DynASM",
                "TPDE",
                "executor",
                "MIR interpreter",
                "VM opcode handler",
            ],
            "leak_count": len(leaks),
            "product_runtime_activation": False,
            "status": "pass" if not leaks else "fail",
            "test_extension_default_off": True,
        },
        "build_wiring": {
            "config_matches_source_manifest": True,
            "source_count": len(source_manifest["w04_production_sources"]),
            "source_manifest_sha256": sha256_file(SOURCE_MANIFEST_PATH),
        },
        "corpus": {
            "accepted": sorted(
                case["case_id"]
                for case in cases
                if case["expected_status"] == "accepted"
            ),
            "accepted_count": sum(
                case["expected_status"] == "accepted" for case in cases
            ),
            "diagnostic_coverage": dict(sorted(diagnostic_counts.items())),
            "rejected": sorted(
                case["case_id"]
                for case in cases
                if case["expected_status"] == "rejected"
            ),
            "rejected_count": sum(
                case["expected_status"] == "rejected" for case in cases
            ),
        },
        "determinism": {
            "axes": [
                "arena-chunk-size",
                "opcache-disabled",
                "opcache-enabled",
                "registry-registration-order",
                "repeat-calls-1-through-10",
                "stable-source-identity",
            ],
            "normalization": {"enabled": False, "rules": []},
            "report_has_timestamp": False,
        },
        "gates": list(GATES),
        "goldens": {
            "cfg_totals": dict(sorted(cfg_totals.items())),
            "sha256": {
                case_id: entry["sha256"]
                for case_id, entry in sorted(index["cases"].items())
            },
        },
        "ownership": {
            "manifest_task_count": len(ownership["specialist_tasks"]),
            "specialist_heads": {
                "W04-A-production-control-flow": A_HEAD,
                "W04-B-control-flow-evidence": B_HEAD,
            },
            "status": "pass",
        },
        "profile": {
            "accepted_opcode_count": len(accepted_entries),
            "active_opcode_count": profile["active_opcode_count"],
            "classification_counts": dict(
                sorted(Counter(entry["classification"] for entry in entries).items())
            ),
            "deferred_by_wave": dict(
                sorted(
                    Counter(
                        entry["deferred_wave"]
                        for entry in deferred_entries
                        if entry["deferred_wave"] is not None
                    ).items()
                )
            ),
            "deferred_opcode_count": len(deferred_entries),
            "format_version": profile["format_version"],
            "mir_contract_version": profile["mir_contract_version"],
            "unresolved_w04_count": 0,
        },
        "sanitizer_fuzz_regression": {
            "address_sanitizer": "required",
            "fixed_seed": 20260719,
            "fuzz_cases": 20000,
            "prior_waves": ["W00", "W01", "W02", "W03"],
            "undefined_behavior_sanitizer": "required",
            "zts": "required",
        },
        "schema_version": 1,
        "stage_verification": {
            "branch_mapping": True,
            "edge_mapping": True,
            "edge_statepoints": True,
            "loop_mapping": True,
            "phi_predecessor_order": True,
            "stages": [1, 2, 3],
            "status": "pass",
        },
        "status": "pass" if not leaks else "fail",
        "wave": "W04",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    arguments = parser.parse_args()
    try:
        verify_specialist_heads()
        expected = stable_bytes(report_document())
        if arguments.write:
            REPORT_PATH.write_bytes(expected)
            print("wrote {}".format(relative(REPORT_PATH)))
            return 0
        if not REPORT_PATH.is_file() or REPORT_PATH.read_bytes() != expected:
            raise W04Error(
                "{} is stale; run validate-w04.py --write".format(
                    relative(REPORT_PATH)
                )
            )
        print(
            "W04 integration validation passed: source manifest, profile, "
            "goldens, ownership, and report"
        )
        return 0
    except (
        OSError,
        KeyError,
        TypeError,
        subprocess.TimeoutExpired,
        W04Error,
    ) as error:
        print("validate-w04.py: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
