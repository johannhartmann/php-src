#!/usr/bin/env python3
"""Verify a committed native-engine wave receipt.

Repository artifacts are read from the receipt's subject commit, not from the
current checkout. External command logs are bound by digest metadata and are
rehashable with --artifact-root.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Dict, List, Optional, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LEDGER = REPO_ROOT / "docs/native-engine/waves/ledger.json"
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
WAVE_RE = re.compile(r"^W(?:0[0-9]|1[0-8])$")
RECEIPT_STATES = {"unsealed", "revalidated", "sealed", "invalid"}


class ReceiptError(Exception):
    pass


def load_json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReceiptError("%s: %s" % (path, exc)) from exc
    if not isinstance(value, dict):
        raise ReceiptError("%s must contain a JSON object" % path)
    return value


def digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    try:
        return digest_bytes(path.read_bytes())
    except OSError as exc:
        raise ReceiptError("%s: %s" % (path, exc)) from exc


def git_output(*arguments: str, binary: bool = False) -> Any:
    process = subprocess.run(
        ["git", *arguments],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        raise ReceiptError("git %s failed: %s" % (" ".join(arguments), detail))
    return process.stdout if binary else process.stdout.decode("utf-8").strip()


def require_sha256(value: Any, field: str, issues: List[str]) -> None:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        issues.append("%s must be a lowercase SHA-256" % field)


def validate_shape(receipt: Dict[str, Any]) -> List[str]:
    issues: List[str] = []
    required = {
        "format_version", "wave_id", "state", "subject_commit",
        "subject_tree", "definition_path", "definition_sha256",
        "profile_paths", "profile_sha256", "coverage_report_path",
        "coverage_report_sha256", "command_manifest", "artifact_digests",
        "review_digests", "dependency_receipts", "capabilities_provided",
        "semantic_debts", "codegen_eligible", "created_at",
    }
    missing = sorted(required - set(receipt))
    if missing:
        issues.append("missing required fields: %s" % ", ".join(missing))
    if receipt.get("format_version") != "1.0.0":
        issues.append("format_version must equal 1.0.0")
    if not isinstance(receipt.get("wave_id"), str) or WAVE_RE.fullmatch(receipt.get("wave_id", "")) is None:
        issues.append("wave_id must be W00 through W18")
    if receipt.get("state") not in RECEIPT_STATES:
        issues.append("state is invalid")
    for field in ("subject_commit", "subject_tree"):
        value = receipt.get(field)
        if not isinstance(value, str) or COMMIT_RE.fullmatch(value) is None:
            issues.append("%s must be a 40-character lowercase hash" % field)
    require_sha256(receipt.get("definition_sha256"), "definition_sha256", issues)
    require_sha256(receipt.get("coverage_report_sha256"), "coverage_report_sha256", issues)
    paths = receipt.get("profile_paths")
    hashes = receipt.get("profile_sha256")
    if not isinstance(paths, list) or not all(isinstance(item, str) and item for item in paths):
        issues.append("profile_paths must be an array of paths")
    if not isinstance(hashes, list):
        issues.append("profile_sha256 must be an array")
    else:
        for index, value in enumerate(hashes):
            require_sha256(value, "profile_sha256[%d]" % index, issues)
    if isinstance(paths, list) and isinstance(hashes, list) and len(paths) != len(hashes):
        issues.append("profile_paths and profile_sha256 lengths differ")
    for field in (
        "command_manifest", "artifact_digests", "review_digests",
        "dependency_receipts", "capabilities_provided", "semantic_debts",
    ):
        if not isinstance(receipt.get(field), list):
            issues.append("%s must be an array" % field)
    if receipt.get("codegen_eligible") is not False:
        issues.append("codegen_eligible must be false")
    if not isinstance(receipt.get("created_at"), str) or "T" not in receipt.get("created_at", ""):
        issues.append("created_at must be an RFC 3339 timestamp")
    return issues


def verify_repository_artifact(
    subject: str, path: Any, expected: Any, field: str, issues: List[str]
) -> None:
    if not isinstance(path, str) or not path or path.startswith("/") or ".." in Path(path).parts:
        issues.append("%s_path must be a repository-relative path" % field)
        return
    try:
        content = git_output("show", "%s:%s" % (subject, path), binary=True)
    except ReceiptError as exc:
        issues.append(str(exc))
        return
    actual = digest_bytes(content)
    if actual != expected:
        issues.append("%s digest mismatch: expected %s, got %s" % (field, expected, actual))


def verify_receipt(
    receipt: Dict[str, Any],
    ledger: Dict[str, Any],
    receipt_path: Path,
    artifact_root: Optional[Path],
) -> List[str]:
    issues = validate_shape(receipt)
    if issues:
        return issues
    subject = receipt["subject_commit"]
    try:
        actual_tree = git_output("rev-parse", "%s^{tree}" % subject)
    except ReceiptError as exc:
        issues.append(str(exc))
    else:
        if actual_tree != receipt["subject_tree"]:
            issues.append(
                "subject_tree mismatch: expected %s, got %s"
                % (receipt["subject_tree"], actual_tree)
            )
    verify_repository_artifact(
        subject, receipt["definition_path"], receipt["definition_sha256"],
        "definition", issues,
    )
    verify_repository_artifact(
        subject, receipt["coverage_report_path"],
        receipt["coverage_report_sha256"], "coverage_report", issues,
    )
    for index, (path, expected) in enumerate(
        zip(receipt["profile_paths"], receipt["profile_sha256"])
    ):
        verify_repository_artifact(
            subject, path, expected, "profile[%d]" % index, issues
        )

    commands: Dict[str, Dict[str, Any]] = {}
    command_artifacts = set()
    for index, item in enumerate(receipt["command_manifest"]):
        if not isinstance(item, dict):
            issues.append("command_manifest[%d] must be an object" % index)
            continue
        command_id = item.get("command_id")
        artifact_id = item.get("artifact_id")
        if not isinstance(command_id, str) or not command_id:
            issues.append("command_manifest[%d].command_id is invalid" % index)
        elif command_id in commands:
            issues.append("duplicate command_id %s" % command_id)
        else:
            commands[command_id] = item
        if not isinstance(item.get("command"), str) or not item["command"]:
            issues.append("command_manifest[%d].command is invalid" % index)
        if item.get("exit_code") != 0:
            issues.append("command_manifest[%d].exit_code must equal 0" % index)
        if not isinstance(artifact_id, str) or not artifact_id:
            issues.append("command_manifest[%d].artifact_id is invalid" % index)
        elif artifact_id in command_artifacts:
            issues.append("log artifact %s is reused by multiple commands" % artifact_id)
        else:
            command_artifacts.add(artifact_id)

    seen_artifacts = set()
    seen_paths = set()
    for index, item in enumerate(receipt["artifact_digests"]):
        if not isinstance(item, dict):
            issues.append("artifact_digests[%d] must be an object" % index)
            continue
        artifact_id = item.get("artifact_id")
        command_id = item.get("command_id")
        path = item.get("path")
        if artifact_id in seen_artifacts:
            issues.append("duplicate artifact_id %s" % artifact_id)
        seen_artifacts.add(artifact_id)
        if command_id not in commands:
            issues.append("artifact %s has unknown command_id %s" % (artifact_id, command_id))
        elif commands[command_id].get("artifact_id") != artifact_id:
            issues.append("artifact %s is not bound by command %s" % (artifact_id, command_id))
        if not isinstance(path, str) or not path or path.startswith("/") or ".." in Path(path).parts:
            issues.append("artifact %s path is invalid" % artifact_id)
        elif path in seen_paths:
            issues.append("artifact path %s is reused" % path)
        else:
            seen_paths.add(path)
        require_sha256(item.get("sha256"), "artifact %s sha256" % artifact_id, issues)
        if not isinstance(item.get("size_bytes"), int) or item["size_bytes"] < 1:
            issues.append("artifact %s size_bytes must be positive" % artifact_id)
        if artifact_root is not None and isinstance(path, str):
            candidate = artifact_root / path
            if not candidate.is_file():
                issues.append("artifact %s is missing: %s" % (artifact_id, candidate))
            else:
                actual_size = candidate.stat().st_size
                actual_digest = digest_file(candidate)
                if actual_size != item.get("size_bytes"):
                    issues.append("artifact %s size mismatch" % artifact_id)
                if actual_digest != item.get("sha256"):
                    issues.append("artifact %s digest mismatch" % artifact_id)
    if command_artifacts != seen_artifacts:
        issues.append("command and artifact identity sets differ")

    entries = ledger.get("waves")
    if not isinstance(entries, list):
        issues.append("ledger.waves must be an array")
    else:
        matches = [entry for entry in entries if isinstance(entry, dict) and entry.get("wave_id") == receipt["wave_id"]]
        if len(matches) != 1:
            issues.append("ledger must contain exactly one %s entry" % receipt["wave_id"])
        else:
            entry = matches[0]
            relative_receipt = receipt_path.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
            if entry.get("state") != receipt["state"]:
                issues.append("ledger and receipt states differ")
            if entry.get("receipt_path") != relative_receipt:
                issues.append("ledger receipt_path does not name this receipt")
            if entry.get("receipt_sha256") != digest_file(receipt_path):
                issues.append("ledger receipt_sha256 does not match this receipt")
            for field in ("capabilities_provided", "semantic_debts", "codegen_eligible"):
                if entry.get(field) != receipt.get(field):
                    issues.append("ledger and receipt %s differ" % field)
    return issues


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wave_id", choices=["W%02d" % number for number in range(19)])
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument(
        "--artifact-root",
        type=Path,
        help="rehash external command logs relative to this directory",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    ledger = load_json(args.ledger)
    receipt_path = args.receipt
    if receipt_path is None:
        entries = ledger.get("waves", [])
        matches = [
            item for item in entries
            if isinstance(item, dict) and item.get("wave_id") == args.wave_id
        ]
        if len(matches) != 1 or not isinstance(matches[0].get("receipt_path"), str):
            print("%s has no committed receipt" % args.wave_id, file=sys.stderr)
            return 1
        receipt_path = REPO_ROOT / matches[0]["receipt_path"]
    receipt = load_json(receipt_path)
    if receipt.get("wave_id") != args.wave_id:
        print("receipt wave_id does not match %s" % args.wave_id, file=sys.stderr)
        return 1
    issues = verify_receipt(receipt, ledger, receipt_path, args.artifact_root)
    if issues:
        print("%s receipt invalid:" % args.wave_id, file=sys.stderr)
        for issue in issues:
            print("- %s" % issue, file=sys.stderr)
        return 1
    print(
        "%s receipt valid: %s at %s"
        % (args.wave_id, receipt["state"], receipt["subject_commit"])
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
