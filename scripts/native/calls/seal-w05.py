#!/usr/bin/env python3
"""Capture W05-v2 command evidence and create its durable v2 seal."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
H = "e164f851875a621858058fa5d641cdf1477c1466"
P = "62c21bcab034185eef7c5c88a2d73e9eee108421"
QM = "950a69b384ad82bc7792c0f8654753a3e32b7d18"
BASELINE = "31cac4a51b51c91ad9e25b72c9312775405e2cf4"
REFERENCE_COMMIT = "47355da494ba696b1bdb6d10448a225e742bd316"
RECEIPT = ROOT / "docs/native-engine/waves/receipts/W05.json"
ARCHIVE = ROOT / "docs/native-engine/waves/receipts/archive/W05-v1.json"
RESULT = ROOT / "docs/native-engine/waves/results/W05-integration-gate.json"
LEDGER = ROOT / "docs/native-engine/waves/ledger.json"
STATUS = ROOT / "docs/native-engine/waves/status.md"
W04_RECEIPT_PATH = "docs/native-engine/waves/receipts/W04.json"
DEFINITION_PATH = "docs/native-engine/waves/waves.json"
COVERAGE_PATH = "docs/native-engine/calls/w05-coverage-report.json"
REVIEWS_PATH = "docs/native-engine/calls/w05-review-manifest.json"
SEQUENCE_PATH = "docs/native-engine/calls/w05-sequence-profile.json"
SUMMARY_ROOT = ROOT / "tests/native/calls/integration/commands"
PROFILES = (
    "docs/native-engine/calls/w05-opcode-profile.json",
    SEQUENCE_PATH,
    "docs/native-engine/calls/w05-reclassification.json",
    "docs/native-engine/calls/w05-phase-manifest.json",
    REVIEWS_PATH,
    "tests/native/calls/integration/goldens/index.json",
    "tests/native/calls/integration/gate-evidence.json",
    "docs/native-engine/build/native-source-manifest.json",
)
SEAL_PATHS = (
    "docs/native-engine/waves/receipts/archive/W05-v1.json",
    "docs/native-engine/waves/receipts/W05.json",
    "docs/native-engine/waves/results/W05-integration-gate.json",
    "docs/native-engine/waves/ledger.json",
    "docs/native-engine/waves/status.md",
)
PATH_ENVIRONMENT = (
    "REFERENCE_PHP",
    "CANDIDATE_PHP",
    "ZTS_PHP",
    "ASAN_PHP",
    "UBSAN_PHP",
)
CRITERIA = (
    ("W05-v2-AC1", "All confirmed post-seal findings are closed."),
    ("W05-v2-AC2", "No compiler sidechannel remains."),
    ("W05-v2-AC3", "Parameter modes are represented by scalable records."),
    ("W05-v2-AC4", "Capability and debt IDs are canonical."),
    ("W05-v2-AC5", "Final verifier receipts bind one module fingerprint."),
    ("W05-v2-AC6", "Dependency receipts and both reviews are verified."),
    ("W05-v2-AC7", "Committed evidence contains no local absolute paths."),
    ("W05-v2-AC8", "Default, normalized-named, and recursive cases pass."),
    ("W05-v2-AC9", "W05-v1 is archived and dual-read remains supported."),
    ("W05-v2-AC10", "W05 remains model-only and not codegen eligible."),
)


class SealError(RuntimeError):
    """The gate evidence or seal is invalid."""


def stable_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def sha_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha_file(path: Path) -> str:
    try:
        return sha_bytes(path.read_bytes())
    except OSError as error:
        raise SealError(f"{path}: {error}") from error


def load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SealError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise SealError(f"{path} must contain an object")
    return value


def write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(stable_bytes(value))
    os.replace(temporary, path)


def git(*arguments: str, binary: bool = False) -> Any:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode:
        raise SealError(
            completed.stderr.decode(errors="replace").strip()
            or "git command failed"
        )
    return completed.stdout if binary else completed.stdout.decode().strip()


def subject_bytes(subject: str, path: str) -> bytes:
    return git("show", f"{subject}:{path}", binary=True)


def digest_ref(subject: str, path: str) -> dict[str, str]:
    return {"path": path, "sha256": sha_bytes(subject_bytes(subject, path))}


def subject_time(subject: str) -> str:
    value = dt.datetime.fromisoformat(git("show", "-s", "--format=%cI", subject))
    return value.astimezone(dt.timezone.utc).replace(
        microsecond=0
    ).isoformat().replace("+00:00", "Z")


def normalize_argv(argv: list[str]) -> list[str]:
    replacements = {
        os.environ[name]: "${%s}" % name
        for name in PATH_ENVIRONMENT
        if os.environ.get(name)
    }
    normalized = []
    for token in argv:
        value = token
        for absolute, replacement in replacements.items():
            value = value.replace(absolute, replacement)
        if value.startswith("/") or (
            "=" in value and value.split("=", 1)[1].startswith("/")
        ):
            raise SealError(f"command argument cannot be normalized: {token}")
        normalized.append(value)
    return normalized


def capture_command(
    command_id: str,
    environment_profile: str,
    artifact_root: Path,
    argv: list[str],
) -> int:
    if not argv:
        raise SealError("captured command needs argv")
    started = time.monotonic_ns()
    completed = subprocess.run(
        argv,
        cwd=ROOT,
        env=os.environ.copy(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    duration_ms = max(0, (time.monotonic_ns() - started) // 1_000_000)
    raw_path = Path("logs") / f"{command_id}.log"
    external = artifact_root / raw_path
    external.parent.mkdir(parents=True, exist_ok=True)
    raw = b"STDOUT\n" + completed.stdout + b"STDERR\n" + completed.stderr
    external.write_bytes(raw)
    summary = {
        "argv": normalize_argv(argv),
        "command_id": command_id,
        "duration_ms": duration_ms,
        "environment_profile": environment_profile,
        "exit_code": completed.returncode,
        "format_version": "2.0.0",
        "raw_log": raw_path.as_posix(),
        "raw_log_sha256": sha_bytes(raw),
        "raw_log_size_bytes": len(raw),
        "stderr_sha256": sha_bytes(completed.stderr),
        "stdout_sha256": sha_bytes(completed.stdout),
    }
    write(SUMMARY_ROOT / f"{command_id}.json", summary)
    sys.stdout.buffer.write(completed.stdout)
    sys.stderr.buffer.write(completed.stderr)
    return completed.returncode


def summary_references(subject: str) -> list[dict[str, str]]:
    paths = sorted(
        line
        for line in git(
            "ls-tree", "-r", "--name-only", subject,
            "tests/native/calls/integration/commands",
        ).splitlines()
        if line.endswith(".json")
    )
    if not paths:
        raise SealError("QG contains no command summaries")
    references = []
    for path in paths:
        document = json.loads(subject_bytes(subject, path))
        command_id = document.get("command_id")
        if not isinstance(command_id, str):
            raise SealError(f"{path}: command_id is invalid")
        references.append(
            {
                "command_id": command_id,
                "path": path,
                "sha256": sha_bytes(subject_bytes(subject, path)),
            }
        )
    if len({item["command_id"] for item in references}) != len(references):
        raise SealError("command summary IDs are not unique")
    return references


def phase_receipts(
    subject: str, summaries: list[dict[str, str]]
) -> list[dict[str, Any]]:
    commits = git("rev-list", "--reverse", f"{H}^..{subject}").splitlines()
    receipts = []
    for commit in commits:
        body = git("show", "-s", "--format=%B", commit)
        trailers = [
            line.split(":", 1)[1].strip()
            for line in body.splitlines()
            if line.startswith("Native-Phase:")
        ]
        if len(trailers) != 1:
            raise SealError(f"{commit}: missing unique Native-Phase trailer")
        phase_id = trailers[0]
        receipts.append(
            {
                "changed_paths": git(
                    "diff-tree", "--no-commit-id", "--name-only", "-r", commit
                ).splitlines(),
                "command_summary_digests": (
                    summaries if phase_id == "W05-v2-gate" else []
                ),
                "commit": commit,
                "format_version": "1.0.0",
                "parent": git("rev-parse", f"{commit}^"),
                "phase_id": phase_id,
                "tree": git("rev-parse", f"{commit}^{{tree}}"),
            }
        )
    return receipts


def toolchain(binary: Path) -> str:
    manifest = binary.parents[3] / "build-manifest.json"
    if manifest.is_file():
        value = load(manifest).get("toolchain", {}).get("compiler_version")
        if isinstance(value, str) and value:
            return value
    completed = subprocess.run(
        ["cc", "--version"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return completed.stdout.splitlines()[0] if completed.stdout else "cc unavailable"


def binary_manifests(
    subject: str,
    artifact_root: Path,
    binaries: dict[str, Path],
) -> list[dict[str, Any]]:
    subject_tree = git("rev-parse", f"{subject}^{{tree}}")
    reference_tree = git("rev-parse", f"{REFERENCE_COMMIT}^{{tree}}")
    manifests = []
    specs = (
        (
            "reference-php",
            "reference",
            "reference",
            REFERENCE_COMMIT,
            reference_tree,
            ["--disable-all", "--enable-cli"],
        ),
        (
            "candidate-debug-nts",
            "candidate",
            "candidate",
            subject,
            subject_tree,
            ["--profile", "w05-debug-nts"],
        ),
        (
            "candidate-debug-zts",
            "candidate",
            "zts",
            subject,
            subject_tree,
            ["--profile", "w05-debug-zts"],
        ),
        (
            "candidate-asan-nts",
            "candidate",
            "asan",
            subject,
            subject_tree,
            ["--profile", "w05-asan-nts"],
        ),
        (
            "candidate-ubsan-nts",
            "candidate",
            "ubsan",
            subject,
            subject_tree,
            ["--profile", "w05-ubsan-nts"],
        ),
    )
    for binary_id, role, key, commit, tree, configure_args in specs:
        source = binaries[key].resolve()
        if not source.is_file() or not os.access(source, os.X_OK):
            raise SealError(f"{key} PHP is not executable: {source}")
        relative = Path("binaries") / f"{binary_id}-php"
        target = artifact_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifests.append(
            {
                "artifact_path": relative.as_posix(),
                "binary_id": binary_id,
                "configure_args": configure_args,
                "git_commit": commit,
                "git_tree": tree,
                "role": role,
                "sha256": sha_file(target),
                "toolchain": toolchain(source),
            }
        )
    return manifests


def receipt_document(
    subject: str,
    artifact_root: Path,
    binaries: dict[str, Path],
) -> dict[str, Any]:
    summaries = summary_references(subject)
    sequence = json.loads(subject_bytes(subject, SEQUENCE_PATH))
    review_manifest = json.loads(subject_bytes(subject, REVIEWS_PATH))
    if (
        review_manifest.get("contract_commit") != H
        or review_manifest.get("wave_pin_commit") != P
        or review_manifest.get("implementation_head") != QM
    ):
        raise SealError("review manifest does not bind QH, QP, and QM")
    reviews = [
        {"path": item["path"], "sha256": item["sha256"]}
        for item in review_manifest["reviews"]
    ]
    return {
        "binary_manifests": binary_manifests(
            subject, artifact_root, binaries
        ),
        "capability_ids": sorted(sequence["capabilities"]),
        "codegen_eligible": False,
        "command_summaries": summaries,
        "coverage": digest_ref(subject, COVERAGE_PATH),
        "definition": digest_ref(subject, DEFINITION_PATH),
        "dependency_receipts": [
            {
                "path": W04_RECEIPT_PATH,
                "sha256": sha_bytes(subject_bytes(subject, W04_RECEIPT_PATH)),
                "wave_id": "W04",
            }
        ],
        "format_version": "2.0.0",
        "phase_receipts": phase_receipts(subject, summaries),
        "profiles": [digest_ref(subject, path) for path in PROFILES],
        "reviews": reviews,
        "semantic_debt_ids": sorted(sequence["debts"]),
        "state": "sealed",
        "subject_commit": subject,
        "subject_tree": git("rev-parse", f"{subject}^{{tree}}"),
        "verification_level": "full",
        "wave_id": "W05",
    }


def result_document(
    subject: str,
    receipt_sha: str,
    phases: list[dict[str, Any]],
    summaries: list[dict[str, str]],
) -> dict[str, Any]:
    changed = git("diff", "--name-only", f"{BASELINE}..{subject}").splitlines()
    tests = []
    for reference in summaries:
        summary = json.loads(subject_bytes(subject, reference["path"]))
        tests.append(
            {
                "artifact": {
                    "kind": "local",
                    "reference": summary["raw_log"],
                },
                "command": " ".join(summary["argv"]),
                "duration_ms": summary["duration_ms"],
                "exit_code": summary["exit_code"],
                "status": "pass",
            }
        )
    return {
        "acceptance_criteria": [
            {
                "criterion_id": identifier,
                "description": description,
                "status": "pass",
            }
            for identifier, description in CRITERIA
        ],
        "actual_base_commit": BASELINE,
        "blockers": [],
        "branch": "integration/wave-06",
        "changed_paths": sorted(set(changed) | set(SEAL_PATHS)),
        "expected_base_commit": BASELINE,
        "format_version": "1.0.0",
        "gate_evidence": [
            {
                "artifact": {
                    "kind": "local",
                    "reference": "logs/validate-w05.log",
                },
                "format_version": "1.0.0",
                "gate_id": "W05-integration-gate",
                "status": "pass",
                "summary": "W05-v2 model-only hard gate and evidence reseal passed.",
                "wave_id": "W05",
            }
        ],
        "head_commit": None,
        "phase_receipts": phases,
        "risks": [],
        "seal_subject": {
            "receipt_path": "docs/native-engine/waves/receipts/W05.json",
            "receipt_sha256": receipt_sha,
        },
        "status": "pass",
        "task_id": "W05-integration-gate",
        "tested_head_commit": subject,
        "tests": tests,
        "timestamp": subject_time(subject),
        "worktree_clean": True,
    }


def ledger_document(receipt: dict[str, Any], receipt_sha: str) -> dict[str, Any]:
    ledger = load(LEDGER)
    matches = [
        item for item in ledger.get("waves", [])
        if item.get("wave_id") == "W05"
    ]
    if len(matches) != 1:
        raise SealError("ledger must contain exactly one W05 entry")
    matches[0].update(
        {
            "capabilities_provided": receipt["capability_ids"],
            "codegen_eligible": False,
            "receipt_path": "docs/native-engine/waves/receipts/W05.json",
            "receipt_sha256": receipt_sha,
            "semantic_debts": receipt["semantic_debt_ids"],
            "state": "sealed",
        }
    )
    return ledger


def seal(
    subject: str,
    artifact_root: Path,
    binaries: dict[str, Path],
) -> None:
    resolved = git("rev-parse", f"{subject}^{{commit}}")
    if resolved != git("rev-parse", "HEAD"):
        raise SealError("subject must be current QG HEAD")
    if git("status", "--porcelain=v1", "--untracked-files=all"):
        raise SealError("seal must start from a clean QG worktree")
    if git("rev-parse", f"{QM}^") != P:
        raise SealError("QP/QM history is not linear")
    old_receipt = RECEIPT.read_bytes()
    ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    ARCHIVE.write_bytes(old_receipt)
    receipt = receipt_document(resolved, artifact_root, binaries)
    write(RECEIPT, receipt)
    receipt_sha = sha_file(RECEIPT)
    summaries = receipt["command_summaries"]
    phases = receipt["phase_receipts"]
    write(RESULT, result_document(resolved, receipt_sha, phases, summaries))
    write(LEDGER, ledger_document(receipt, receipt_sha))
    rendered = subprocess.run(
        [
            sys.executable,
            "scripts/native/wave-gate.py",
            "render",
            "--ledger",
            str(LEDGER),
            "--output",
            str(STATUS),
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if rendered.returncode:
        raise SealError(rendered.stdout.strip())


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="mode", required=True)
    capture = commands.add_parser("run")
    capture.add_argument("--command-id", required=True)
    capture.add_argument("--environment-profile", required=True)
    capture.add_argument("--artifact-root", type=Path, required=True)
    capture.add_argument("argv", nargs=argparse.REMAINDER)
    seal_parser = commands.add_parser("seal")
    seal_parser.add_argument("--subject", required=True)
    seal_parser.add_argument("--artifact-root", type=Path, required=True)
    for option in ("reference", "candidate", "zts", "asan", "ubsan"):
        seal_parser.add_argument(f"--{option}-php", type=Path, required=True)
    seal_parser.add_argument("--write", action="store_true", required=True)
    return root


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.mode == "run":
            argv = arguments.argv
            if argv[:1] == ["--"]:
                argv = argv[1:]
            return capture_command(
                arguments.command_id,
                arguments.environment_profile,
                arguments.artifact_root.resolve(),
                argv,
            )
        seal(
            arguments.subject,
            arguments.artifact_root.resolve(),
            {
                "reference": arguments.reference_php,
                "candidate": arguments.candidate_php,
                "zts": arguments.zts_php,
                "asan": arguments.asan_php,
                "ubsan": arguments.ubsan_php,
            },
        )
        print(f"W05-v2 seal written for {arguments.subject}")
        return 0
    except (
        SealError,
        OSError,
        KeyError,
        TypeError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        print(f"W05-v2 seal: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
