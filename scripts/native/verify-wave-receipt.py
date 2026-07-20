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
LEVELS = ("metadata", "reproducible", "full")


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
    receipt_content: Optional[bytes] = None,
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
            relative_receipt = (
                receipt_path.as_posix()
                if not receipt_path.is_absolute()
                else receipt_path.resolve().relative_to(
                    REPO_ROOT.resolve()
                ).as_posix()
            )
            if entry.get("state") != receipt["state"]:
                issues.append("ledger and receipt states differ")
            if entry.get("receipt_path") != relative_receipt:
                issues.append("ledger receipt_path does not name this receipt")
            receipt_digest = (
                digest_bytes(receipt_content)
                if receipt_content is not None
                else digest_file(receipt_path)
            )
            if entry.get("receipt_sha256") != receipt_digest:
                issues.append("ledger receipt_sha256 does not match this receipt")
            for field in ("capabilities_provided", "semantic_debts", "codegen_eligible"):
                if entry.get(field) != receipt.get(field):
                    issues.append("ledger and receipt %s differ" % field)
    return issues


def is_relative_path(value: Any) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and not value.startswith("/")
        and not re.match(r"^[A-Za-z]:[\\/]", value)
        and ".." not in Path(value).parts
    )


def contains_absolute_path_token(value: Any) -> bool:
    if not isinstance(value, str):
        return True
    candidates = [value]
    if "=" in value:
        candidates.append(value.split("=", 1)[1])
    return any(
        candidate.startswith("/")
        or re.match(r"^[A-Za-z]:[\\/]", candidate)
        for candidate in candidates
    )


def read_subject_artifact(
    subject: str, reference: Any, label: str, issues: List[str]
) -> Optional[bytes]:
    if not isinstance(reference, dict):
        issues.append("%s must be an object" % label)
        return None
    path = reference.get("path")
    expected = reference.get("sha256")
    if not is_relative_path(path):
        issues.append("%s.path must be repository-relative" % label)
        return None
    require_sha256(expected, "%s.sha256" % label, issues)
    try:
        content = git_output("show", "%s:%s" % (subject, path), binary=True)
    except ReceiptError as exc:
        issues.append(str(exc))
        return None
    if digest_bytes(content) != expected:
        issues.append("%s digest mismatch" % label)
    return content


def validate_review(
    content: bytes, expected_subject: str, label: str, issues: List[str]
) -> Optional[Dict[str, Any]]:
    try:
        review = json.loads(content)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        issues.append("%s is not valid JSON: %s" % (label, exc))
        return
    required = {
        "format_version", "review_id", "review_kind", "subject_commit",
        "subject_tree", "reviewer", "status", "findings",
    }
    if not isinstance(review, dict) or set(review) != required:
        issues.append("%s is missing required review fields" % label)
        return None
    if review["format_version"] != "2.0.0":
        issues.append("%s format_version is not 2.0.0" % label)
    if (
        not isinstance(review["review_id"], str)
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*", review["review_id"]) is None
    ):
        issues.append("%s review_id is invalid" % label)
    if review["review_kind"] not in {
        "semantics", "evidence-history-capability"
    }:
        issues.append("%s review_kind is invalid" % label)
    if not isinstance(review["reviewer"], str) or not review["reviewer"].strip():
        issues.append("%s reviewer is empty" % label)
    if review["subject_commit"] != expected_subject:
        issues.append("%s subject_commit differs from QM subject" % label)
    if review["status"] not in {"pass", "fail"}:
        issues.append("%s status is invalid" % label)
    if not isinstance(review["findings"], list):
        issues.append("%s findings must be an array" % label)
    elif review["status"] != "pass" or review["findings"]:
        issues.append("%s is not a blocker-free review" % label)
    try:
        tree = git_output("rev-parse", "%s^{tree}" % review["subject_commit"])
    except ReceiptError as exc:
        issues.append(str(exc))
    else:
        if review["subject_tree"] != tree:
            issues.append("%s subject_tree mismatch" % label)
    return review


def validate_digest_reference(
    reference: Any, label: str, issues: List[str]
) -> None:
    if not isinstance(reference, dict):
        issues.append("%s must be an object" % label)
        return
    if set(reference) != {"path", "sha256"}:
        issues.append("%s must contain only path and sha256" % label)
    if not is_relative_path(reference.get("path")):
        issues.append("%s.path must be repository-relative" % label)
    require_sha256(reference.get("sha256"), "%s.sha256" % label, issues)


def validate_command_summary_reference(
    reference: Any, label: str, issues: List[str]
) -> None:
    if not isinstance(reference, dict):
        issues.append("%s must be an object" % label)
        return
    if set(reference) != {"command_id", "path", "sha256"}:
        issues.append(
            "%s must contain only command_id, path and sha256" % label
        )
    if (
        not isinstance(reference.get("command_id"), str)
        or re.fullmatch(
            r"[a-z0-9][a-z0-9-]*", reference.get("command_id", "")
        ) is None
    ):
        issues.append("%s.command_id is invalid" % label)
    if not is_relative_path(reference.get("path")):
        issues.append("%s.path must be repository-relative" % label)
    require_sha256(reference.get("sha256"), "%s.sha256" % label, issues)


def validate_command_summary(
    summary: Any, label: str, issues: List[str]
) -> Optional[Dict[str, Any]]:
    required = {
        "format_version", "command_id", "argv", "environment_profile",
        "exit_code", "stdout_sha256", "stderr_sha256", "duration_ms",
    }
    optional = {"raw_log", "raw_log_sha256", "raw_log_size_bytes"}
    if not isinstance(summary, dict):
        issues.append("%s must be an object" % label)
        return None
    if not required.issubset(summary) or not set(summary).issubset(
        required | optional
    ):
        issues.append("%s has an invalid field set" % label)
    command_id = summary.get("command_id")
    if (
        not isinstance(command_id, str)
        or re.fullmatch(r"[a-z0-9][a-z0-9-]*", command_id) is None
    ):
        issues.append("%s command_id is invalid" % label)
    if summary.get("format_version") != "2.0.0":
        issues.append("%s format_version is invalid" % label)
    argv = summary.get("argv")
    if not isinstance(argv, list) or not argv:
        issues.append("%s has no argv" % label)
    elif any(
        not isinstance(token, str)
        or contains_absolute_path_token(token)
        for token in argv
    ):
        issues.append("%s contains an absolute path" % label)
    if (
        not isinstance(summary.get("environment_profile"), str)
        or re.fullmatch(
            r"[a-z0-9][a-z0-9-]*",
            summary.get("environment_profile", ""),
        ) is None
    ):
        issues.append("%s environment_profile is invalid" % label)
    exit_code = summary.get("exit_code")
    if (
        not isinstance(exit_code, int)
        or isinstance(exit_code, bool)
        or exit_code != 0
    ):
        issues.append("%s did not pass with an integer exit code" % label)
    for field in ("stdout_sha256", "stderr_sha256"):
        require_sha256(summary.get(field), "%s %s" % (label, field), issues)
    duration = summary.get("duration_ms")
    if (
        not isinstance(duration, int)
        or isinstance(duration, bool)
        or duration < 0
    ):
        issues.append("%s duration_ms is invalid" % label)
    raw_fields = optional & set(summary)
    if raw_fields and raw_fields != optional:
        issues.append("%s raw-log fields must be present together" % label)
    if raw_fields == optional:
        if not is_relative_path(summary.get("raw_log")):
            issues.append("%s raw_log is invalid" % label)
        require_sha256(
            summary.get("raw_log_sha256"),
            "%s raw_log_sha256" % label,
            issues,
        )
        raw_size = summary.get("raw_log_size_bytes")
        if (
            not isinstance(raw_size, int)
            or isinstance(raw_size, bool)
            or raw_size < 1
        ):
            issues.append("%s raw_log_size_bytes is invalid" % label)
    return summary


def validate_binary_manifests(
    manifests: Any,
    issues: List[str],
    subject_commit: Optional[str] = None,
    subject_tree: Optional[str] = None,
) -> None:
    if not isinstance(manifests, list):
        return
    binary_ids = set()
    roles = set()
    for index, manifest in enumerate(manifests):
        label = "binary_manifests[%d]" % index
        required = {
            "binary_id", "role", "artifact_path", "sha256", "git_commit",
            "git_tree", "configure_args", "toolchain",
        }
        if not isinstance(manifest, dict) or set(manifest) != required:
            issues.append("%s has an invalid field set" % label)
            continue
        binary_id = manifest["binary_id"]
        if (
            not isinstance(binary_id, str)
            or re.fullmatch(r"[a-z0-9][a-z0-9-]*", binary_id) is None
            or binary_id in binary_ids
        ):
            issues.append("%s binary_id is invalid or duplicated" % label)
        binary_ids.add(binary_id)
        if manifest["role"] not in {"reference", "candidate"}:
            issues.append("%s role is invalid" % label)
        roles.add(manifest["role"])
        if manifest["role"] == "candidate":
            if (
                subject_commit is not None
                and manifest["git_commit"] != subject_commit
            ):
                issues.append(
                    "%s candidate commit differs from receipt subject" % label
                )
            if (
                subject_tree is not None
                and manifest["git_tree"] != subject_tree
            ):
                issues.append(
                    "%s candidate tree differs from receipt subject" % label
                )
        if not is_relative_path(manifest["artifact_path"]):
            issues.append("%s artifact_path is invalid" % label)
        require_sha256(manifest["sha256"], "%s.sha256" % label, issues)
        for field in ("git_commit", "git_tree"):
            if (
                not isinstance(manifest[field], str)
                or COMMIT_RE.fullmatch(manifest[field]) is None
            ):
                issues.append("%s %s is invalid" % (label, field))
        configure_args = manifest["configure_args"]
        if (
            not isinstance(configure_args, list)
            or any(
                not isinstance(token, str)
                or contains_absolute_path_token(token)
                for token in configure_args
            )
        ):
            issues.append("%s configure_args contain an absolute path" % label)
        if (
            not isinstance(manifest["toolchain"], str)
            or not manifest["toolchain"].strip()
        ):
            issues.append("%s toolchain is empty" % label)
    if roles != {"reference", "candidate"}:
        issues.append("binary manifests need reference and candidate roles")


def validate_v2_shape(receipt: Dict[str, Any]) -> List[str]:
    issues: List[str] = []
    required = {
        "format_version", "wave_id", "state", "verification_level",
        "subject_commit", "subject_tree", "definition", "profiles", "coverage",
        "command_summaries", "binary_manifests", "reviews",
        "dependency_receipts", "phase_receipts", "capability_ids",
        "semantic_debt_ids", "codegen_eligible",
    }
    missing = sorted(required - set(receipt))
    if missing:
        issues.append("missing required fields: %s" % ", ".join(missing))
    unknown = sorted(set(receipt) - required)
    if unknown:
        issues.append("unknown fields: %s" % ", ".join(unknown))
    if receipt.get("format_version") != "2.0.0":
        issues.append("format_version must equal 2.0.0")
    if (
        not isinstance(receipt.get("wave_id"), str)
        or WAVE_RE.fullmatch(receipt.get("wave_id", "")) is None
    ):
        issues.append("wave_id must be W00 through W18")
    if receipt.get("verification_level") not in LEVELS:
        issues.append("verification_level is invalid")
    if receipt.get("state") not in RECEIPT_STATES:
        issues.append("state is invalid")
    if receipt.get("codegen_eligible") is not False:
        issues.append("codegen_eligible must be false")
    for field in ("subject_commit", "subject_tree"):
        if not isinstance(receipt.get(field), str) or COMMIT_RE.fullmatch(receipt.get(field, "")) is None:
            issues.append("%s must be a commit hash" % field)
    for field in (
        "profiles", "command_summaries", "binary_manifests", "reviews",
        "dependency_receipts", "phase_receipts", "capability_ids",
        "semantic_debt_ids",
    ):
        if not isinstance(receipt.get(field), list):
            issues.append("%s must be an array" % field)
    for field in ("definition", "coverage"):
        if not isinstance(receipt.get(field), dict):
            issues.append("%s must be a digest reference" % field)
        else:
            validate_digest_reference(receipt[field], field, issues)
    dependencies = receipt.get("dependency_receipts")
    if isinstance(dependencies, list):
        for index, dependency in enumerate(dependencies):
            label = "dependency_receipts[%d]" % index
            if (
                not isinstance(dependency, dict)
                or set(dependency) != {"wave_id", "path", "sha256"}
            ):
                issues.append("%s has an invalid field set" % label)
                continue
            if (
                not isinstance(dependency["wave_id"], str)
                or WAVE_RE.fullmatch(dependency["wave_id"]) is None
            ):
                issues.append("%s.wave_id is invalid" % label)
            if not is_relative_path(dependency["path"]):
                issues.append("%s.path is invalid" % label)
            require_sha256(
                dependency["sha256"], "%s.sha256" % label, issues
            )
    for field, minimum in (
        ("command_summaries", 1),
        ("binary_manifests", 2),
        ("reviews", 2),
        ("phase_receipts", 4),
    ):
        values = receipt.get(field)
        if isinstance(values, list) and len(values) < minimum:
            issues.append("%s must contain at least %d entries" % (field, minimum))
    for field in ("profiles", "reviews"):
        values = receipt.get(field)
        if isinstance(values, list):
            for index, reference in enumerate(values):
                validate_digest_reference(
                    reference, "%s[%d]" % (field, index), issues
                )
    for index, reference in enumerate(receipt.get("command_summaries", [])):
        validate_command_summary_reference(
            reference, "command_summaries[%d]" % index, issues
        )
    validate_binary_manifests(
        receipt.get("binary_manifests"),
        issues,
        receipt.get("subject_commit"),
        receipt.get("subject_tree"),
    )
    for field in ("capability_ids", "semantic_debt_ids"):
        values = receipt.get(field)
        if isinstance(values, list) and (
            any(
                not isinstance(value, str)
                or re.fullmatch(r"[a-z][a-z0-9_]*", value) is None
                for value in values
            )
            or values != sorted(set(values))
        ):
            issues.append("%s must contain sorted unique IDs" % field)
    return issues


def validate_phase_receipts(receipt: Dict[str, Any], issues: List[str]) -> None:
    actual: List[str] = []
    phase_summary_refs: List[tuple[str, str, str]] = []
    for index, phase in enumerate(receipt["phase_receipts"]):
        if not isinstance(phase, dict):
            issues.append("phase_receipts[%d] must be an object" % index)
            continue
        required = {
            "format_version", "phase_id", "commit", "tree", "parent",
            "changed_paths", "command_summary_digests",
        }
        if set(phase) != required:
            issues.append(
                "phase_receipts[%d] has an invalid field set" % index
            )
        if phase.get("format_version") != "1.0.0":
            issues.append(
                "phase_receipts[%d].format_version is invalid" % index
            )
        phase_id = phase.get("phase_id")
        actual.append(phase_id)
        if (
            not isinstance(phase_id, str)
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*", phase_id) is None
        ):
            issues.append("phase_receipts[%d].phase_id is invalid" % index)
        commit = phase.get("commit")
        for field in ("commit", "tree", "parent"):
            if not isinstance(phase.get(field), str) or COMMIT_RE.fullmatch(phase.get(field, "")) is None:
                issues.append("phase_receipts[%d].%s is invalid" % (index, field))
        if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
            continue
        try:
            tree = git_output("rev-parse", "%s^{tree}" % commit)
            parent = git_output("rev-parse", "%s^" % commit)
            body = git_output("show", "-s", "--format=%B", commit)
            paths = git_output(
                "diff-tree", "--no-commit-id", "--name-only", "-r", commit
            ).splitlines()
        except ReceiptError as exc:
            issues.append(str(exc))
            continue
        if phase.get("tree") != tree or phase.get("parent") != parent:
            issues.append("phase receipt %s commit binding mismatch" % phase_id)
        if phase.get("changed_paths") != paths:
            issues.append("phase receipt %s changed_paths mismatch" % phase_id)
        changed_paths = phase.get("changed_paths")
        if (
            not isinstance(changed_paths, list)
            or any(not is_relative_path(path) for path in changed_paths)
            or len(changed_paths) != len(set(changed_paths))
        ):
            issues.append(
                "phase receipt %s changed_paths are invalid" % phase_id
            )
        summaries = phase.get("command_summary_digests")
        if not isinstance(summaries, list):
            issues.append("phase receipt %s summaries must be an array" % phase_id)
        else:
            for summary_index, reference in enumerate(summaries):
                validate_command_summary_reference(
                    reference,
                    "phase receipt %s summary[%d]" % (phase_id, summary_index),
                    issues,
                )
                if isinstance(reference, dict):
                    command_id = reference.get("command_id")
                    path = reference.get("path")
                    digest = reference.get("sha256")
                    if all(
                        isinstance(value, str)
                        for value in (command_id, path, digest)
                    ):
                        phase_summary_refs.append(
                            (command_id, path, digest)
                        )
        trailers = [
            line.split(":", 1)[1].strip()
            for line in body.splitlines()
            if line.startswith("Native-Phase:")
        ]
        if trailers != [phase_id]:
            issues.append("phase receipt %s trailer mismatch" % phase_id)
    if (
        len(actual) < 4
        or actual[:2] != ["W05-v2-contract", "W05-v2-wave-pin"]
        or actual[-1:] != ["W05-v2-gate"]
        or any(
            phase_id != "W05-v2-implementation" for phase_id in actual[2:-1]
        )
    ):
        issues.append("phase receipt order must be QH, QP, QM, QG")
    receipt_summary_refs = [
        (
            reference.get("command_id"),
            reference.get("path"),
            reference.get("sha256"),
        )
        for reference in receipt["command_summaries"]
        if isinstance(reference, dict)
    ]
    if (
        len(phase_summary_refs) != len(set(phase_summary_refs))
        or sorted(phase_summary_refs) != sorted(receipt_summary_refs)
    ):
        issues.append(
            "phase command summary digests must bind every summary exactly once"
        )


def verify_v2_receipt(
    receipt: Dict[str, Any],
    ledger: Dict[str, Any],
    receipt_path: Path,
    level: str,
    artifact_root: Optional[Path],
    visited: Optional[set[str]] = None,
    receipt_content: Optional[bytes] = None,
) -> List[str]:
    issues = validate_v2_shape(receipt)
    if issues:
        return issues
    subject = receipt["subject_commit"]
    try:
        actual_tree = git_output("rev-parse", "%s^{tree}" % subject)
    except ReceiptError as exc:
        issues.append(str(exc))
    else:
        if actual_tree != receipt["subject_tree"]:
            issues.append("subject_tree mismatch")
    if LEVELS.index(level) > LEVELS.index(receipt["verification_level"]):
        issues.append("receipt only claims %s verification" % receipt["verification_level"])
    if level == "metadata":
        return issues

    read_subject_artifact(subject, receipt["definition"], "definition", issues)
    read_subject_artifact(subject, receipt["coverage"], "coverage", issues)
    for index, ref in enumerate(receipt["profiles"]):
        read_subject_artifact(subject, ref, "profiles[%d]" % index, issues)

    command_ids = set()
    raw_logs: List[tuple[str, Dict[str, Any]]] = []
    for index, ref in enumerate(receipt["command_summaries"]):
        content = read_subject_artifact(
            subject, ref, "command_summaries[%d]" % index, issues
        )
        if content is None:
            continue
        try:
            summary = json.loads(content)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            issues.append("command summary is invalid JSON: %s" % exc)
            continue
        summary_label = "command summary %s" % ref.get(
            "command_id", index
        )
        if validate_command_summary(summary, summary_label, issues) is None:
            continue
        command_id = summary.get("command_id")
        reference_id = ref.get("command_id")
        if reference_id is not None and reference_id != command_id:
            issues.append(
                "command_summaries[%d] command_id differs from summary" % index
            )
        if command_id in command_ids:
            issues.append("duplicate command summary ID %s" % command_id)
        command_ids.add(command_id)
        raw = summary.get("raw_log")
        if raw is not None:
            raw_logs.append((command_id, summary))

    implementation_commits = [
        phase.get("commit")
        for phase in receipt["phase_receipts"]
        if isinstance(phase, dict)
        and phase.get("phase_id") == "W05-v2-implementation"
    ]
    implementation_subject = (
        implementation_commits[-1] if implementation_commits else ""
    )
    review_kinds = set()
    review_ids = set()
    for index, ref in enumerate(receipt["reviews"]):
        content = read_subject_artifact(subject, ref, "reviews[%d]" % index, issues)
        if content is not None:
            review = validate_review(
                content, implementation_subject, "reviews[%d]" % index, issues
            )
            if review is not None:
                review_kinds.add(review.get("review_kind"))
                review_ids.add(review.get("review_id"))
    if review_kinds != {"semantics", "evidence-history-capability"}:
        issues.append("reviews must contain both required review kinds")
    if len(review_ids) != len(receipt["reviews"]):
        issues.append("review IDs must be unique")

    for index, manifest in enumerate(receipt["binary_manifests"]):
        if not isinstance(manifest, dict):
            continue
        try:
            expected_tree = git_output(
                "rev-parse", "%s^{tree}" % manifest.get("git_commit", "")
            )
        except ReceiptError as exc:
            issues.append(str(exc))
        else:
            if manifest.get("git_tree") != expected_tree:
                issues.append(
                    "binary_manifests[%d] git tree mismatch" % index
                )

    registry_path = "docs/native-engine/roadmap/capability-registry.json"
    try:
        registry = json.loads(
            git_output("show", "%s:%s" % (subject, registry_path), binary=True)
        )
        known = {
            kind: {
                entry["id"] for entry in registry["entries"]
                if entry.get("kind") == kind
            }
            for kind in ("capability", "semantic_debt")
        }
    except (
        ReceiptError, KeyError, TypeError, UnicodeDecodeError,
        json.JSONDecodeError,
    ) as exc:
        issues.append("capability registry invalid: %s" % exc)
        known = {"capability": set(), "semantic_debt": set()}
    for field, kind in (
        ("capability_ids", "capability"),
        ("semantic_debt_ids", "semantic_debt"),
    ):
        values = receipt[field]
        if values != sorted(values) or len(values) != len(set(values)):
            issues.append("%s must be sorted and unique" % field)
        unknown = sorted(set(values) - known[kind])
        if unknown:
            issues.append("%s contains unknown IDs: %s" % (field, ", ".join(unknown)))

    visited = set() if visited is None else visited
    visited.add(receipt["wave_id"])
    for index, dependency in enumerate(receipt["dependency_receipts"]):
        if not isinstance(dependency, dict):
            issues.append("dependency_receipts[%d] must be an object" % index)
            continue
        path = dependency.get("path")
        if not is_relative_path(path):
            issues.append("dependency_receipts[%d].path is invalid" % index)
            continue
        try:
            content = git_output(
                "show", "%s:%s" % (subject, path), binary=True
            )
        except ReceiptError as exc:
            issues.append(str(exc))
            continue
        if digest_bytes(content) != dependency.get("sha256"):
            issues.append("dependency receipt digest mismatch: %s" % path)
            continue
        try:
            child = json.loads(content)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            issues.append("dependency receipt is invalid JSON: %s" % exc)
            continue
        if not isinstance(child, dict):
            issues.append("dependency receipt must be an object: %s" % path)
            continue
        dependency_path = Path(path)
        child_wave = child.get("wave_id")
        if child_wave != dependency.get("wave_id"):
            issues.append("dependency wave ID mismatch: %s" % path)
        elif child_wave in visited:
            issues.append("dependency receipt cycle at %s" % child_wave)
        elif child.get("format_version") == "2.0.0":
            child_visited = set(visited)
            issues.extend(
                verify_v2_receipt(
                    child, ledger, dependency_path, "reproducible", None,
                    child_visited, content,
                )
            )
        else:
            issues.extend(
                verify_receipt(
                    child, ledger, dependency_path, None, content
                )
            )

    validate_phase_receipts(receipt, issues)
    if level == "full":
        if artifact_root is None:
            issues.append("--artifact-root is required for full verification")
        else:
            for command_id, summary in raw_logs:
                path = artifact_root / summary["raw_log"]
                if not path.is_file():
                    issues.append("raw log missing for %s" % command_id)
                else:
                    if digest_file(path) != summary.get("raw_log_sha256"):
                        issues.append("raw log digest mismatch for %s" % command_id)
                    if path.stat().st_size != summary.get("raw_log_size_bytes"):
                        issues.append("raw log size mismatch for %s" % command_id)
            for manifest in receipt["binary_manifests"]:
                if not isinstance(manifest, dict):
                    issues.append("binary manifest must be an object")
                    continue
                path = manifest.get("artifact_path")
                if not is_relative_path(path):
                    issues.append("binary artifact_path is invalid")
                    continue
                binary = artifact_root / path
                if not binary.is_file():
                    issues.append("binary is missing: %s" % path)
                elif digest_file(binary) != manifest.get("sha256"):
                    issues.append("binary digest mismatch: %s" % path)
    entries = ledger.get("waves")
    if isinstance(entries, list):
        matches = [
            entry for entry in entries
            if isinstance(entry, dict) and entry.get("wave_id") == receipt["wave_id"]
        ]
        if len(matches) == 1:
            entry = matches[0]
            relative = (
                receipt_path.as_posix()
                if not receipt_path.is_absolute()
                else receipt_path.resolve().relative_to(
                    REPO_ROOT.resolve()
                ).as_posix()
            )
            if entry.get("receipt_path") != relative:
                issues.append("ledger receipt_path does not name this receipt")
            receipt_digest = (
                digest_bytes(receipt_content)
                if receipt_content is not None
                else digest_file(receipt_path)
            )
            if entry.get("receipt_sha256") != receipt_digest:
                issues.append("ledger receipt digest mismatch")
            for receipt_field, ledger_field in (
                ("state", "state"),
                ("capability_ids", "capabilities_provided"),
                ("semantic_debt_ids", "semantic_debts"),
                ("codegen_eligible", "codegen_eligible"),
            ):
                if entry.get(ledger_field) != receipt.get(receipt_field):
                    issues.append(
                        "ledger and receipt %s differ" % receipt_field
                    )
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
    parser.add_argument("--level", choices=LEVELS, default="metadata")
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
    if receipt.get("format_version") == "2.0.0":
        issues = verify_v2_receipt(
            receipt, ledger, receipt_path, args.level, args.artifact_root
        )
    else:
        issues = verify_receipt(
            receipt,
            ledger,
            receipt_path,
            args.artifact_root if args.level == "full" else None,
        )
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
