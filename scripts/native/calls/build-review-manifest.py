#!/usr/bin/env python3
"""Build or validate the deterministic W05-v2 review manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "docs/native-engine/calls/w05-review-manifest.json"
REVIEW_ROOT = ROOT / "docs/native-engine/reviews/W05-v2"
SEMANTICS = REVIEW_ROOT / "semantics.json"
EVIDENCE = REVIEW_ROOT / "evidence-history-capability.json"
CONTRACT_COMMIT = "e164f851875a621858058fa5d641cdf1477c1466"
WAVE_PIN_COMMIT = "62c21bcab034185eef7c5c88a2d73e9eee108421"
IMPLEMENTATION_HEAD = "950a69b384ad82bc7792c0f8654753a3e32b7d18"
REVIEW_KINDS = (
    ("semantics", SEMANTICS),
    ("evidence-history-capability", EVIDENCE),
)


class ReviewManifestError(RuntimeError):
    """The external reviews cannot authorize the W05-v2 gate."""


def stable_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def sha256(path: Path) -> str:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ReviewManifestError(f"{path}: {error}") from error
    if not payload:
        raise ReviewManifestError(f"review artifact is empty: {path}")
    return hashlib.sha256(payload).hexdigest()


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReviewManifestError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise ReviewManifestError(f"{path} must contain a JSON object")
    return value


def git(*arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode:
        raise ReviewManifestError(
            completed.stderr.strip() or "git command failed"
        )
    return completed.stdout.strip()


def validate_review(
    path: Path, kind: str, implementation_head: str
) -> dict[str, Any]:
    document = load_object(path)
    required = {
        "format_version",
        "review_id",
        "review_kind",
        "subject_commit",
        "subject_tree",
        "reviewer",
        "status",
        "findings",
    }
    if set(document) != required:
        raise ReviewManifestError(f"{path}: invalid review field set")
    if document["format_version"] != "2.0.0":
        raise ReviewManifestError(f"{path}: review format is not v2")
    if document["review_kind"] != kind:
        raise ReviewManifestError(f"{path}: unexpected review kind")
    if document["subject_commit"] != implementation_head:
        raise ReviewManifestError(f"{path}: review subject differs from QM")
    if document["subject_tree"] != git(
        "rev-parse", f"{implementation_head}^{{tree}}"
    ):
        raise ReviewManifestError(f"{path}: review tree differs from QM")
    if document["status"] != "pass" or document["findings"] != []:
        raise ReviewManifestError(f"{path}: review is not blocker-free")
    if not isinstance(document["reviewer"], str) or not document["reviewer"]:
        raise ReviewManifestError(f"{path}: reviewer is empty")
    return document


def build_document(
    implementation_head: str = IMPLEMENTATION_HEAD,
) -> dict[str, Any]:
    if implementation_head != IMPLEMENTATION_HEAD:
        raise ReviewManifestError("implementation_head must equal reviewed QM")
    if git("rev-parse", f"{WAVE_PIN_COMMIT}^") != CONTRACT_COMMIT:
        raise ReviewManifestError("QH/QP history is not linear")
    if git("merge-base", "--is-ancestor", WAVE_PIN_COMMIT, implementation_head):
        raise ReviewManifestError("QP is not an ancestor of QM")
    reviews = []
    ids: set[str] = set()
    for kind, path in REVIEW_KINDS:
        document = validate_review(path, kind, implementation_head)
        review_id = document["review_id"]
        if review_id in ids:
            raise ReviewManifestError("review IDs must be unique")
        ids.add(review_id)
        reviews.append(
            {
                "path": path.relative_to(ROOT).as_posix(),
                "review_id": review_id,
                "review_kind": kind,
                "sha256": sha256(path),
            }
        )
    return {
        "contract_commit": CONTRACT_COMMIT,
        "format_version": "2.0.0",
        "implementation_head": implementation_head,
        "implementation_tree": git(
            "rev-parse", f"{implementation_head}^{{tree}}"
        ),
        "reviews": reviews,
        "wave_pin_commit": WAVE_PIN_COMMIT,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    if arguments.check == arguments.write:
        parser.error("choose exactly one of --check or --write")
    try:
        expected = stable_bytes(build_document())
        if arguments.write:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            temporary = arguments.output.with_name(arguments.output.name + ".tmp")
            temporary.write_bytes(expected)
            os.replace(temporary, arguments.output)
        elif (
            not arguments.output.is_file()
            or arguments.output.read_bytes() != expected
        ):
            raise ReviewManifestError("committed review manifest is stale")
        print("W05-v2 review manifest: PASS")
        return 0
    except (ReviewManifestError, OSError, TypeError, KeyError) as error:
        print(f"W05-v2 review manifest: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
