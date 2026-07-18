#!/usr/bin/env python3
"""Validate W03 lowering contracts and maintain its timestamp-free report."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
LOWERING = ROOT / "Zend/Native/Lowering"
PROFILE_PATH = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"
BLOCKERS_PATH = ROOT / "docs/native-engine/lowering/w03-blockers.json"
MANIFEST_PATH = ROOT / "tests/native/lowering/corpus/manifest.json"
REPORT_PATH = ROOT / "docs/native-engine/lowering/w03-coverage-report.json"

PROVIDER_PATHS = {
    "W03-A-lowering-core-registry": (
        "Zend/Native/Lowering/Core/zend_mir_lowering_registry.c",
        "Zend/Native/Lowering/Core/zend_mir_lowering_providers.c",
    ),
    "W03-B-frontend-operands-facts": (
        "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c",
        "Zend/Native/Lowering/Frontend/zend_mir_value_facts.c",
    ),
    "W03-C-numeric-arithmetic-bitwise": (
        "Zend/Native/Lowering/Scalar/Numeric/zend_mir_numeric_provider.c",
    ),
    "W03-D-comparison-boolean-casts": (
        "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic_provider.c",
    ),
    "W03-E-straight-line-lifetime-return": (
        "Zend/Native/Lowering/StraightLine/zend_mir_lifetime_provider.c",
    ),
}
INTEGRATION_PATHS = (
    "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.c",
    "Zend/Native/Lowering/Core/zend_mir_lowering_providers.c",
    "ext/native_mir_test/config.m4",
    "ext/native_mir_test/native_mir_test.c",
    "scripts/native/lowering/dump-mir.py",
    "scripts/native/lowering/run-w03-differential.py",
)
GATES = (
    "python3 scripts/native/semantics/validate-w01.py --check",
    "python3 scripts/native/mir/validate-w02.py --check",
    "python3 scripts/native/lowering/check-contract.py --check",
    "python3 scripts/native/lowering/validate-w03.py --check",
    "python3 scripts/native/lowering/test-w03.py --reference-php ${REFERENCE_PHP} --candidate-php ${CANDIDATE_PHP}",
    "python3 scripts/native/lowering/test-w03.py --reference-php ${REFERENCE_PHP} --candidate-php ${ASAN_CANDIDATE_PHP} --sanitizer address",
    "python3 scripts/native/lowering/test-w03.py --reference-php ${REFERENCE_PHP} --candidate-php ${UBSAN_CANDIDATE_PHP} --sanitizer undefined",
    "python3 -m unittest discover -s tests/native/lowering -p test_*.py -v",
    "python3 tests/native/lowering/fuzz/run_fuzz.py --seed 20260718 --cases 20000",
    "scripts/native/build.sh --profile debug-nts",
    "scripts/native/test-smoke.sh --profile debug-nts",
    "scripts/native/build.sh --profile debug-zts",
    "scripts/native/test-smoke.sh --profile debug-zts",
)
FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*"
    r"(?:tpde|dynasm|x86|amd64|aarch64|arm64|riscv|"
    r"elf|macho|coff|zend_jit|zend_vm_execute)[^>\"]*[>\"]",
    re.IGNORECASE | re.MULTILINE,
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:TPDE|DynASM|zend_execute|execute_ex|zend_vm_call_opcode_handler|"
    r"mir_interpret|mir_evaluate)\b",
    re.IGNORECASE,
)


class W03Error(RuntimeError):
    """A deterministic W03 validation failure."""


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise W03Error(f"{relative(path)}: {error}") from error


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def production_paths() -> tuple[Path, ...]:
    paths = [
        *LOWERING.rglob("*.c"),
        *LOWERING.rglob("*.h"),
        ROOT / "ext/native_mir_test/config.m4",
        ROOT / "ext/native_mir_test/native_mir_test.c",
        ROOT / "ext/native_mir_test/native_mir_test.stub.php",
    ]
    return tuple(sorted(paths))


def tree_digest(paths: tuple[Path, ...]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(relative(path).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def architecture_leaks() -> list[str]:
    leaks: list[str] = []
    for path in production_paths():
        text = path.read_text(encoding="utf-8")
        include = FORBIDDEN_INCLUDE.search(text)
        runtime = FORBIDDEN_RUNTIME.search(text)
        if include is not None:
            leaks.append(
                f"{relative(path)}: forbidden include {include.group(0).strip()}"
            )
        elif runtime is not None:
            leaks.append(
                f"{relative(path)}: forbidden runtime token {runtime.group(0)}"
            )
    return leaks


def validate_profile(profile: dict[str, Any]) -> None:
    entries = profile.get("opcodes")
    if not isinstance(entries, list) or len(entries) != profile.get(
        "active_opcode_count"
    ):
        raise W03Error("profile opcode count does not match active_opcode_count")
    numbers = [entry.get("number") for entry in entries]
    if numbers != sorted(numbers) or len(numbers) != len(set(numbers)):
        raise W03Error("profile opcode numbers are not sorted and unique")
    proof_catalog = set(profile.get("proof_catalog", []))
    accepted = [entry for entry in entries if entry.get("classification") != "deferred"]
    if not accepted:
        raise W03Error("profile has no accepted opcodes")
    for entry in entries:
        if entry.get("classification") not in {"required", "conditional", "deferred"}:
            raise W03Error(f"invalid classification for {entry.get('opcode')}")
        unknown = set(entry.get("proofs", [])) - proof_catalog
        if unknown:
            raise W03Error(
                f"{entry.get('opcode')} references unknown proofs: {sorted(unknown)}"
            )
        owner = entry.get("owner")
        if entry.get("classification") != "deferred":
            if owner not in PROVIDER_PATHS:
                raise W03Error(f"accepted opcode has no W03 provider owner: {owner}")
            if entry.get("deferred_wave") is not None:
                raise W03Error(f"accepted opcode is also deferred: {entry.get('opcode')}")
        elif entry.get("mir_opcodes"):
            raise W03Error(f"deferred opcode publishes MIR: {entry.get('opcode')}")
    for owner in sorted({entry["owner"] for entry in accepted}):
        for value in PROVIDER_PATHS[owner]:
            if not (ROOT / value).is_file():
                raise W03Error(f"missing provider implementation: {value}")
    for value in INTEGRATION_PATHS:
        if not (ROOT / value).is_file():
            raise W03Error(f"missing integration path: {value}")


def report_document() -> dict[str, Any]:
    profile = load_json(PROFILE_PATH)
    manifest = load_json(MANIFEST_PATH)
    blockers = load_json(BLOCKERS_PATH)
    validate_profile(profile)
    entries = profile["opcodes"]
    accepted_entries = [
        entry for entry in entries if entry["classification"] != "deferred"
    ]
    deferred_entries = [
        entry for entry in entries if entry["classification"] == "deferred"
    ]
    cases = manifest["cases"]
    owner_counts = Counter(entry["owner"] for entry in accepted_entries)
    proof_counts = Counter(
        proof for entry in accepted_entries for proof in entry["proofs"]
    )
    diagnostic_counts = Counter(case["expected_mirl"] for case in cases)
    paths = production_paths()
    leaks = architecture_leaks()
    return {
        "architecture_independence": {
            "forbidden_families": [
                "DynASM",
                "TPDE",
                "architecture-specific headers",
                "executor",
                "VM handler",
            ],
            "leak_count": len(leaks),
            "product_runtime_activation": False,
            "status": "pass" if not leaks else "fail",
            "test_extension_default_off": True,
        },
        "blockers": {
            "open_count": len(blockers.get("blockers", [])),
            "resolved_contract_drifts": blockers.get(
                "resolved_contract_drifts", []
            ),
        },
        "corpus": {
            "accepted": sorted(
                case["case_id"]
                for case in cases
                if case["disposition"] == "accepted"
            ),
            "accepted_count": sum(
                case["disposition"] == "accepted" for case in cases
            ),
            "diagnostic_coverage": dict(sorted(diagnostic_counts.items())),
            "golden_sha256": {
                case["case_id"]: case["golden_sha256"]
                for case in cases
                if case["golden_sha256"] is not None
            },
            "rejected": sorted(
                case["case_id"]
                for case in cases
                if case["disposition"] == "rejected"
            ),
            "rejected_count": sum(
                case["disposition"] == "rejected" for case in cases
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
        "profile": {
            "accepted_opcode_count": len(accepted_entries),
            "active_opcode_count": profile["active_opcode_count"],
            "classification_counts": dict(
                sorted(Counter(entry["classification"] for entry in entries).items())
            ),
            "deferred_opcode_count": len(deferred_entries),
            "deferred_opcodes": [entry["opcode"] for entry in deferred_entries],
            "format_version": profile["format_version"],
            "mir_contract_version": profile["mir_contract_version"],
        },
        "proof_coverage": {
            proof: {
                "accepted_opcode_count": proof_counts.get(proof, 0),
                "status": "covered" if proof_counts.get(proof, 0) else "not-required",
            }
            for proof in profile["proof_catalog"]
        },
        "provider_coverage": {
            owner: {
                "claimed_opcode_count": owner_counts.get(owner, 0),
                "implementation_paths": list(PROVIDER_PATHS[owner]),
                "status": "covered" if owner_counts.get(owner, 0) else "not-required",
            }
            for owner in sorted(PROVIDER_PATHS)
        },
        "regressions": {
            "required_waves": ["W00", "W01", "W02"],
            "standard_build_runtime_path": False,
        },
        "schema_version": 1,
        "source_inventory": {
            "combined_sha256": tree_digest(paths),
            "path_count": len(paths),
            "paths": [relative(path) for path in paths],
        },
        "wave": "W03",
    }


def report_bytes() -> bytes:
    return (
        json.dumps(report_document(), indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        check=True,
        timeout=180,
    )


def validate_report() -> None:
    expected = report_bytes()
    if not REPORT_PATH.is_file():
        raise W03Error(f"missing deterministic report: {relative(REPORT_PATH)}")
    if REPORT_PATH.read_bytes() != expected:
        raise W03Error(
            "W03 coverage report is stale; run "
            "python3 scripts/native/lowering/validate-w03.py --write-report"
        )
    if expected != report_bytes():
        raise W03Error("W03 report generation is nondeterministic")


def validate() -> None:
    leaks = architecture_leaks()
    if leaks:
        raise W03Error("\n".join(leaks))
    profile = load_json(PROFILE_PATH)
    validate_profile(profile)
    blockers = load_json(BLOCKERS_PATH)
    if blockers.get("blockers") != []:
        raise W03Error("W03 blockers file contains unresolved blockers")
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
    )
    run(
        ["python3", "scripts/native/lowering/generate-profile.py", "--check"],
        environment,
    )
    run(
        ["python3", "tests/native/lowering/corpus/validate_manifest.py", "--check"],
        environment,
    )
    run(
        ["python3", "scripts/native/lowering/check-contract.py", "--check"],
        environment,
    )
    validate_report()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write-report", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.write_report:
            REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
            REPORT_PATH.write_bytes(report_bytes())
            print(f"wrote {relative(REPORT_PATH)}")
        else:
            validate()
            print("W03 lowering validation passed")
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        W03Error,
    ) as error:
        print(f"W03 lowering validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
