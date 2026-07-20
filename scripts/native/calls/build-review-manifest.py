#!/usr/bin/env python3
"""Build or validate the deterministic W05 independent-review manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "docs/native-engine/calls/w05-review-manifest.json"
CONTRACT_COMMIT = "63a5070daa91da9e702d0ff52ea4d77c20ad89e6"
WAVE_PIN_COMMIT = "8833e6a7be1bd5fa8b9b5da972512ba798db4e33"
CONTRACT_INPUTS = {
    "call_sequence_contract": ROOT
    / "docs/native-engine/calls/contracts/call-sequence.md",
    "opcode_profile": ROOT
    / "docs/native-engine/calls/w05-opcode-profile.json",
    "sequence_profile": ROOT
    / "docs/native-engine/calls/w05-sequence-profile.json",
    "reclassification": ROOT
    / "docs/native-engine/calls/w05-reclassification.json",
    "phase_manifest": ROOT
    / "docs/native-engine/calls/w05-phase-manifest.json",
}


class ReviewManifestError(RuntimeError):
    """The external reviews cannot authorize the W05 gate."""


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


def stable_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def is_ancestor(ancestor: str, descendant: str) -> bool:
    completed = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        cwd=ROOT,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    if completed.returncode not in (0, 1):
        raise ReviewManifestError(
            completed.stderr.decode(errors="replace").strip()
            or "git merge-base failed"
        )
    return completed.returncode == 0


def reviewed_head(document: dict[str, Any]) -> str | None:
    """Return the unambiguous implementation head named by a review."""
    candidates = (
        document.get("reviewed_head"),
        document.get("commit"),
        document.get("implementation_head"),
        document.get("commit_chain", {}).get("implementation_head_m")
        if isinstance(document.get("commit_chain"), dict)
        else None,
    )
    heads = {
        value
        for value in candidates
        if isinstance(value, str)
        and len(value) == 40
        and not set(value) - set("0123456789abcdef")
    }
    if len(heads) != 1:
        return None
    return heads.pop()


def review_entry(
    review_id: str,
    json_path: Path,
    markdown_path: Path,
    implementation_head: str,
) -> dict[str, Any]:
    document = load_object(json_path)
    if document.get("approved") is not True:
        raise ReviewManifestError(f"{review_id} is not approved")
    if reviewed_head(document) != implementation_head:
        raise ReviewManifestError(
            f"{review_id} review commit does not match implementation_head"
        )
    findings = document.get("findings")
    if not isinstance(findings, list) or findings:
        raise ReviewManifestError(f"{review_id} retains review findings")
    markdown_digest = sha256(markdown_path)
    json_digest = sha256(json_path)
    embedded = document.get("artifact")
    if isinstance(embedded, dict):
        expected = embedded.get("markdown_sha256")
        if expected is not None and expected != markdown_digest:
            raise ReviewManifestError(
                f"{review_id} embedded Markdown digest does not match"
            )
    hashes = document.get("hashes")
    if isinstance(hashes, dict):
        expected = hashes.get("markdown_review")
        if expected is not None and expected != markdown_digest:
            raise ReviewManifestError(
                f"{review_id} embedded Markdown digest does not match"
            )
    return {
        "approved": True,
        "artifact_names": {
            "json": json_path.name,
            "markdown": markdown_path.name,
        },
        "json_sha256": json_digest,
        "markdown_sha256": markdown_digest,
        "review_id": review_id,
        "reviewed_head": implementation_head,
    }


def build_document(
    implementation_head: str,
    r1_json: Path,
    r1_markdown: Path,
    r2_json: Path,
    r2_markdown: Path,
) -> dict[str, Any]:
    if len(implementation_head) != 40 or any(
        character not in "0123456789abcdef" for character in implementation_head
    ):
        raise ReviewManifestError("implementation_head must be a full Git hash")
    if (
        not is_ancestor(WAVE_PIN_COMMIT, implementation_head)
        or not is_ancestor(implementation_head, "HEAD")
    ):
        raise ReviewManifestError(
            "implementation_head is outside the current W05 history"
        )
    inputs = {
        name: {
            "path": path.relative_to(ROOT).as_posix(),
            "sha256": sha256(path),
        }
        for name, path in sorted(CONTRACT_INPUTS.items())
    }
    return {
        "contract_commit": CONTRACT_COMMIT,
        "contract_inputs": inputs,
        "format_version": "1.0.0",
        "implementation_head": implementation_head,
        "reviews": [
            review_entry(
                "W05-R1", r1_json, r1_markdown, implementation_head
            ),
            review_entry(
                "W05-R2", r2_json, r2_markdown, implementation_head
            ),
        ],
        "wave_pin_commit": WAVE_PIN_COMMIT,
    }


def validate_committed(document: dict[str, Any]) -> None:
    if document.get("format_version") != "1.0.0":
        raise ReviewManifestError("review manifest format_version drifted")
    if document.get("contract_commit") != CONTRACT_COMMIT:
        raise ReviewManifestError("review manifest contract commit drifted")
    if document.get("wave_pin_commit") != WAVE_PIN_COMMIT:
        raise ReviewManifestError("review manifest wave pin commit drifted")
    implementation_head = document.get("implementation_head")
    if not isinstance(implementation_head, str) or len(implementation_head) != 40:
        raise ReviewManifestError("review manifest implementation_head is invalid")
    if (
        not is_ancestor(WAVE_PIN_COMMIT, implementation_head)
        or not is_ancestor(implementation_head, "HEAD")
    ):
        raise ReviewManifestError(
            "review manifest implementation_head is outside W05 history"
        )
    reviews = document.get("reviews")
    if not isinstance(reviews, list) or [item.get("review_id") for item in reviews] != [
        "W05-R1",
        "W05-R2",
    ]:
        raise ReviewManifestError("review manifest must bind R1 and R2 exactly")
    for review in reviews:
        if (
            review.get("approved") is not True
            or review.get("reviewed_head") != implementation_head
        ):
            raise ReviewManifestError("review approval/head binding is invalid")
        for key in ("json_sha256", "markdown_sha256"):
            value = review.get(key)
            if (
                not isinstance(value, str)
                or len(value) != 64
                or set(value) - set("0123456789abcdef")
            ):
                raise ReviewManifestError(f"review {key} is invalid")
    expected_inputs = {
        name: {
            "path": path.relative_to(ROOT).as_posix(),
            "sha256": sha256(path),
        }
        for name, path in sorted(CONTRACT_INPUTS.items())
    }
    if document.get("contract_inputs") != expected_inputs:
        raise ReviewManifestError("review manifest contract input drift")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--implementation-head")
    parser.add_argument("--r1-json", type=Path)
    parser.add_argument("--r1-markdown", type=Path)
    parser.add_argument("--r2-json", type=Path)
    parser.add_argument("--r2-markdown", type=Path)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.check:
            validate_committed(load_object(arguments.output))
            print("W05 review manifest: PASS")
            return 0
        required = (
            arguments.implementation_head,
            arguments.r1_json,
            arguments.r1_markdown,
            arguments.r2_json,
            arguments.r2_markdown,
        )
        if any(value is None for value in required):
            parser.error(
                "generation requires --implementation-head and all R1/R2 paths"
            )
        document = build_document(
            arguments.implementation_head,
            arguments.r1_json,
            arguments.r1_markdown,
            arguments.r2_json,
            arguments.r2_markdown,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_bytes(stable_bytes(document))
        print(f"wrote {arguments.output}")
        return 0
    except ReviewManifestError as error:
        print(f"W05 review manifest: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
