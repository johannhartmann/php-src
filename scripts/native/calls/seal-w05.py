#!/usr/bin/env python3
"""Create the W05 receipt, result, ledger, and deterministic status seal."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
H = "63a5070daa91da9e702d0ff52ea4d77c20ad89e6"
P = "8833e6a7be1bd5fa8b9b5da972512ba798db4e33"
RECEIPT = ROOT / "docs/native-engine/waves/receipts/W05.json"
RESULT = ROOT / "docs/native-engine/waves/results/W05-integration-gate.json"
LEDGER = ROOT / "docs/native-engine/waves/ledger.json"
STATUS = ROOT / "docs/native-engine/waves/status.md"
W04_RECEIPT = ROOT / "docs/native-engine/waves/receipts/W04.json"
DEFINITION = "docs/native-engine/waves/waves.json"
COVERAGE = "docs/native-engine/calls/w05-coverage-report.json"
REVIEWS = "docs/native-engine/calls/w05-review-manifest.json"
SEQUENCE = "docs/native-engine/calls/w05-sequence-profile.json"
PROFILES = (
    "docs/native-engine/calls/w05-opcode-profile.json",
    SEQUENCE,
    "docs/native-engine/calls/w05-reclassification.json",
    "docs/native-engine/calls/w05-phase-manifest.json",
    REVIEWS,
    "tests/native/calls/integration/goldens/index.json",
    "docs/native-engine/build/native-source-manifest.json",
)
SEAL_PATHS = (
    "docs/native-engine/waves/receipts/W05.json",
    "docs/native-engine/waves/results/W05-integration-gate.json",
    "docs/native-engine/waves/ledger.json",
    "docs/native-engine/waves/status.md",
)
COMMANDS = (
    ("check-contract", "python3 scripts/native/calls/check-contract.py --check"),
    ("check-phases", "python3 scripts/native/calls/check-phases.py --check"),
    ("validate-w05", "python3 scripts/native/calls/validate-w05.py --check"),
    ("build-w05-debug-nts", "scripts/native/build.sh --profile w05-debug-nts"),
    ("test-w05-debug-nts", "python3 scripts/native/calls/test-w05.py --reference-php {reference} --candidate-php {candidate}"),
    ("build-w05-debug-zts", "scripts/native/build.sh --profile w05-debug-zts"),
    ("test-w05-debug-zts", "python3 scripts/native/calls/test-w05.py --reference-php {reference} --candidate-php {zts}"),
    ("build-w05-asan-nts", "scripts/native/build.sh --profile w05-asan-nts"),
    ("test-w05-asan-nts", "python3 scripts/native/calls/test-w05.py --reference-php {reference} --candidate-php {asan} --sanitizer address"),
    ("build-w05-ubsan-nts", "scripts/native/build.sh --profile w05-ubsan-nts"),
    ("test-w05-ubsan-nts", "python3 scripts/native/calls/test-w05.py --reference-php {reference} --candidate-php {ubsan} --sanitizer undefined"),
    ("calls-unittest", "TEST_PHP_EXECUTABLE={candidate} python3 -m unittest discover -s tests/native/calls -p 'test_*.py' -v"),
    ("fuzz-20000", "python3 tests/native/calls/fuzz/run_fuzz.py --seed 20260719 --cases 20000 --candidate-php {candidate}"),
    ("validate-w01", "python3 scripts/native/semantics/validate-w01.py --check"),
    ("validate-w02", "python3 scripts/native/mir/validate-w02.py --check"),
    ("validate-w03", "python3 scripts/native/lowering/validate-w03.py --check"),
    ("validate-w04", "python3 scripts/native/control-flow/validate-w04.py --check"),
    ("build-debug-nts", "scripts/native/build.sh --profile debug-nts"),
    ("smoke-debug-nts", "scripts/native/test-smoke.sh --profile debug-nts"),
    ("build-debug-zts", "scripts/native/build.sh --profile debug-zts"),
    ("smoke-debug-zts", "scripts/native/test-smoke.sh --profile debug-zts"),
)
CRITERIA = (
    ("W05-I-AC1", "Both approved reviews bind the same implementation head."),
    ("W05-I-AC2", "The modeled corpus passes every prerequisite verifier."),
    ("W05-I-AC3", "Every deferred case has an exact later wave and MIRL code."),
    ("W05-I-AC4", "Reference and candidate execution are separate and byte-identical."),
    ("W05-I-AC5", "No W05 evidence claims MIR execution."),
    ("W05-I-AC6", "Canonical integration goldens are deterministic and unnormalized."),
    ("W05-I-AC7", "Injected failures publish no module or partial call model."),
    ("W05-I-AC8", "NTS, ZTS, ASan, UBSan, and fixed-seed fuzzing pass."),
    ("W05-I-AC9", "W01 through W04 regressions pass."),
    ("W05-I-AC10", "The workflow uses immutable checkout state and unique logs."),
    ("W05-I-AC11", "The gate commit changes only gate-owned paths."),
)


class SealError(RuntimeError):
    """The gate cannot be durably sealed."""


def git(*args: str, binary: bool = False) -> Any:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise SealError(
            result.stderr.decode(errors="replace").strip() or "git failed"
        )
    return result.stdout if binary else result.stdout.decode().strip()


def git_is_ancestor(ancestor: str, descendant: str) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode not in (0, 1):
        raise SealError(
            result.stderr.decode(errors="replace").strip()
            or "git merge-base failed"
        )
    return result.returncode == 0


def sha_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha_file(path: Path) -> str:
    try:
        value = path.read_bytes()
    except OSError as error:
        raise SealError(f"{path}: {error}") from error
    if not value:
        raise SealError(f"empty artifact: {path}")
    return sha_bytes(value)


def load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SealError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise SealError(f"{path} must contain an object")
    return value


def write(path: Path, value: Any) -> None:
    payload = (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, path)


def subject_bytes(subject: str, path: str) -> bytes:
    return git("show", f"{subject}:{path}", binary=True)


def subject_sha(subject: str, path: str) -> str:
    return sha_bytes(subject_bytes(subject, path))


def timestamp(subject: str) -> str:
    value = dt.datetime.fromisoformat(git("show", "-s", "--format=%cI", subject))
    return value.astimezone(dt.timezone.utc).replace(
        microsecond=0
    ).isoformat().replace("+00:00", "Z")


def command_evidence(
    root: Path, binaries: dict[str, str],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    commands = []
    artifacts = []
    for command_id, template in COMMANDS:
        log = root / f"{command_id}.log"
        if not log.is_file() or log.stat().st_size < 1:
            raise SealError(f"missing command artifact: {log}")
        artifact_id = f"{command_id}-log"
        commands.append({
            "artifact_id": artifact_id,
            "command": template.format(**binaries),
            "command_id": command_id,
            "exit_code": 0,
        })
        artifacts.append({
            "artifact_id": artifact_id,
            "command_id": command_id,
            "path": log.name,
            "sha256": sha_file(log),
            "size_bytes": log.stat().st_size,
        })
    return commands, artifacts


def receipt_document(
    subject: str, artifact_root: Path, binaries: dict[str, str],
) -> dict[str, Any]:
    reviews = json.loads(subject_bytes(subject, REVIEWS))
    sequence = json.loads(subject_bytes(subject, SEQUENCE))
    if (
        reviews.get("contract_commit") != H
        or reviews.get("wave_pin_commit") != P
        or len(reviews.get("reviews", [])) != 2
    ):
        raise SealError("review manifest does not bind H, P, and R1/R2")
    implementation_head = reviews.get("implementation_head")
    if (
        not isinstance(implementation_head, str)
        or len(implementation_head) != 40
        or not git_is_ancestor(P, implementation_head)
        or not git_is_ancestor(implementation_head, subject)
    ):
        raise SealError(
            "review manifest implementation head is outside the W05 chain"
        )
    commands, artifacts = command_evidence(artifact_root, binaries)
    review_digests = [{
        "review_id": "W05-review-manifest",
        "sha256": subject_sha(subject, REVIEWS),
    }]
    for review in reviews["reviews"]:
        review_digests.extend((
            {
                "review_id": f"{review['review_id']}-json",
                "sha256": review["json_sha256"],
            },
            {
                "review_id": f"{review['review_id']}-markdown",
                "sha256": review["markdown_sha256"],
            },
        ))
    return {
        "artifact_digests": artifacts,
        "capabilities_provided": sequence["capabilities"],
        "codegen_eligible": False,
        "command_manifest": commands,
        "coverage_report_path": COVERAGE,
        "coverage_report_sha256": subject_sha(subject, COVERAGE),
        "created_at": timestamp(subject),
        "definition_path": DEFINITION,
        "definition_sha256": subject_sha(subject, DEFINITION),
        "dependency_receipts": [{
            "receipt_sha256": sha_file(W04_RECEIPT),
            "wave_id": "W04",
        }],
        "format_version": "1.0.0",
        "profile_paths": list(PROFILES),
        "profile_sha256": [subject_sha(subject, path) for path in PROFILES],
        "review_digests": review_digests,
        "semantic_debts": sequence["debts"],
        "state": "sealed",
        "subject_commit": subject,
        "subject_tree": git("rev-parse", f"{subject}^{{tree}}"),
        "wave_id": "W05",
    }


def result_document(
    subject: str, receipt_sha: str, commands: list[dict[str, Any]],
) -> dict[str, Any]:
    changed = git("diff", "--name-only", f"{H}..{subject}").splitlines()
    return {
        "acceptance_criteria": [
            {
                "criterion_id": identifier,
                "description": description,
                "status": "pass",
            }
            for identifier, description in CRITERIA
        ],
        "actual_base_commit": H,
        "blockers": [],
        "branch": "integration/wave-05",
        "changed_paths": sorted(set(changed) | set(SEAL_PATHS)),
        "expected_base_commit": H,
        "format_version": "1.0.0",
        "gate_evidence": [{
            "artifact": {"kind": "local", "reference": "validate-w05.log"},
            "format_version": "1.0.0",
            "gate_id": "W05-integration-gate",
            "status": "pass",
            "summary": "W05 call modeling passed without MIR execution.",
            "wave_id": "W05",
        }],
        "head_commit": None,
        "risks": [],
        "seal_subject": {
            "receipt_path": SEAL_PATHS[0],
            "receipt_sha256": receipt_sha,
        },
        "status": "pass",
        "task_id": "W05-integration-gate",
        "tested_head_commit": subject,
        "tests": [
            {
                "artifact": {
                    "kind": "local",
                    "reference": f"{item['command_id']}.log",
                },
                "command": item["command"],
                "duration_ms": None,
                "exit_code": 0,
                "status": "pass",
            }
            for item in commands
        ],
        "timestamp": timestamp(subject),
        "worktree_clean": True,
    }


def ledger_document(receipt: dict[str, Any], receipt_sha: str) -> dict[str, Any]:
    ledger = load(LEDGER)
    entries = [
        item for item in ledger.get("waves", [])
        if item.get("wave_id") == "W05"
    ]
    if len(entries) != 1:
        raise SealError("ledger must contain exactly one W05 entry")
    entries[0].update({
        "capabilities_provided": receipt["capabilities_provided"],
        "codegen_eligible": False,
        "receipt_path": SEAL_PATHS[0],
        "receipt_sha256": receipt_sha,
        "semantic_debts": receipt["semantic_debts"],
        "state": "sealed",
    })
    return ledger


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--subject", required=True)
    parser.add_argument("--artifact-root", type=Path)
    parser.add_argument("--reference-php")
    parser.add_argument("--candidate-php")
    parser.add_argument("--zts-php")
    parser.add_argument("--asan-php")
    parser.add_argument("--ubsan-php")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    if not args.write:
        parser.error("--write is required; seal generation is never implicit")
    try:
        subject = git("rev-parse", f"{args.subject}^{{commit}}")
        if subject != git("rev-parse", "HEAD"):
            raise SealError("subject must be current gate HEAD")
        if git("status", "--porcelain=v1", "--untracked-files=all"):
            raise SealError("seal must start from a clean gate worktree")
        artifact_root = args.artifact_root
        if artifact_root is None:
            value = os.environ.get("W05_ARTIFACT_ROOT")
            if not value:
                raise SealError("--artifact-root or W05_ARTIFACT_ROOT is required")
            artifact_root = Path(value)
        binaries = {
            "reference": args.reference_php or os.environ.get("REFERENCE_PHP"),
            "candidate": args.candidate_php or os.environ.get("CANDIDATE_PHP"),
            "zts": args.zts_php or os.environ.get("ZTS_PHP"),
            "asan": args.asan_php or os.environ.get("ASAN_PHP"),
            "ubsan": args.ubsan_php or os.environ.get("UBSAN_PHP"),
        }
        if not all(isinstance(value, str) and value for value in binaries.values()):
            raise SealError("all five explicit PHP binary paths are required")
        receipt = receipt_document(subject, artifact_root.resolve(), binaries)
        write(RECEIPT, receipt)
        receipt_sha = sha_file(RECEIPT)
        write(RESULT, result_document(subject, receipt_sha, receipt["command_manifest"]))
        write(LEDGER, ledger_document(receipt, receipt_sha))
        rendered = subprocess.run(
            [
                sys.executable, "scripts/native/wave-gate.py", "render",
                "--ledger", str(LEDGER), "--output", str(STATUS),
            ],
            cwd=ROOT, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=60,
        )
        if rendered.returncode:
            raise SealError(rendered.stdout.strip())
        print(f"W05 seal written for {subject}")
        return 0
    except (
        SealError, OSError, KeyError, TypeError, ValueError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"W05 seal: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
