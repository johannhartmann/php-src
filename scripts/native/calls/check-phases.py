#!/usr/bin/env python3
"""Validate the frozen W05 serial phase ownership contract."""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs/native-engine/calls/w05-phase-manifest.json"

EXPECTED_PHASES = (
    "program",
    "contract-H",
    "wave-P",
    "implementation",
    "review-R1",
    "review-R2",
    "gate",
    "seal",
)
EXPECTED_SHARED = (
    "docs/native-engine/waves/ledger.json",
    "docs/native-engine/waves/status.md",
)
EXPECTED_TASKS = {
    "program": ("W05-00",),
    "contract-H": ("W05-0",),
    "wave-P": ("W05-0",),
    "implementation": ("W05-A", "W05-AR"),
    "review-R1": ("W05-R1",),
    "review-R2": ("W05-R2",),
    "gate": ("W05-I",),
    "seal": ("W05-S",),
}


class PhaseError(RuntimeError):
    """The serial W05 phase contract was violated."""


def has_magic(pattern: str) -> bool:
    return any(character in pattern for character in "*?[")


def path_matches(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        prefix = pattern[:-3].rstrip("/")
        return path == prefix or path.startswith(prefix + "/")
    return fnmatchcase(path, pattern)


def patterns_overlap(left: str, right: str) -> bool:
    if left == right:
        return True
    if left.endswith("/**"):
        prefix = left[:-3].rstrip("/")
        if right.endswith("/**"):
            other = right[:-3].rstrip("/")
            return (
                prefix == other
                or prefix.startswith(other + "/")
                or other.startswith(prefix + "/")
            )
        if not has_magic(right):
            return path_matches(right, left)
        return right.startswith(prefix + "/")
    if right.endswith("/**"):
        return patterns_overlap(right, left)
    if not has_magic(left) and not has_magic(right):
        return False
    if not has_magic(left):
        return fnmatchcase(left, right)
    if not has_magic(right):
        return fnmatchcase(right, left)
    left_parent = left.rsplit("/", 1)[0] if "/" in left else ""
    right_parent = right.rsplit("/", 1)[0] if "/" in right else ""
    return left_parent == right_parent


def changed_paths(base: str, head: str) -> list[str]:
    completed = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}", "--"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise PhaseError(completed.stderr.strip() or "git diff failed")
    return [line for line in completed.stdout.splitlines() if line]


def validate_manifest(manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if manifest.get("format_version") != 1:
        errors.append("phase manifest format must be version 1")
    if manifest.get("writer_branch") != "integration/wave-05":
        errors.append("W05 must have exactly the integration/wave-05 writer branch")
    if tuple(manifest.get("shared_serial_paths", [])) != EXPECTED_SHARED:
        errors.append("only ledger.json and status.md may be shared serial paths")

    phases = manifest.get("phases", [])
    phase_ids = tuple(phase.get("phase_id") for phase in phases)
    if phase_ids != EXPECTED_PHASES:
        errors.append("phase identities and order do not match the frozen W05 program")
        return errors

    phase_by_id = {phase["phase_id"]: phase for phase in phases}
    for phase_id, task_ids in EXPECTED_TASKS.items():
        if tuple(phase_by_id[phase_id].get("task_ids", [])) != task_ids:
            errors.append(
                f"{phase_id}: task IDs must be exactly {', '.join(task_ids)}"
            )
    if phase_by_id["review-R1"].get("order") != phase_by_id["review-R2"].get("order"):
        errors.append("the two read-only reviews must share one logical order")
    if any(
        phase_by_id[phase_id].get("write_mode") != "read_only"
        or phase_by_id[phase_id].get("paths")
        for phase_id in ("review-R1", "review-R2")
    ):
        errors.append("review-R1 and review-R2 must be pathless and read-only")
    if any(
        phase.get("write_mode") != "single_writer"
        for phase in phases
        if phase["phase_id"] not in {"review-R1", "review-R2"}
    ):
        errors.append("every writing phase must be single_writer")

    mutable = [
        phase["phase_id"]
        for phase in phases
        if phase.get("root_contracts_mutable")
    ]
    if mutable != ["contract-H"]:
        errors.append("root contracts may be mutable only in contract-H")

    shared = set(EXPECTED_SHARED)
    writers = [
        phase for phase in phases
        if phase.get("write_mode") == "single_writer"
    ]
    for index, left_phase in enumerate(writers):
        for right_phase in writers[index + 1:]:
            for left in left_phase.get("paths", []):
                for right in right_phase.get("paths", []):
                    if not patterns_overlap(left, right):
                        continue
                    if left == right and left in shared:
                        continue
                    errors.append(
                        f"phase path overlap: {left_phase['phase_id']}:{left} and "
                        f"{right_phase['phase_id']}:{right}"
                    )

    providers: dict[str, int] = {}
    for phase in phases:
        order = phase.get("order")
        if not isinstance(order, int):
            errors.append(f"{phase['phase_id']}: order must be an integer")
            continue
        for command in phase.get("commands_provided", []):
            if command in providers:
                errors.append(f"command has multiple providers: {command}")
            providers[command] = order
    for phase in phases:
        order = phase.get("order")
        if not isinstance(order, int):
            continue
        for command in phase.get("commands_used", []):
            provider_order = providers.get(command)
            if provider_order is None:
                errors.append(f"{phase['phase_id']}: command has no provider: {command}")
            elif provider_order >= order:
                errors.append(
                    f"{phase['phase_id']}: command used before provider phase: {command}"
                )

    implementation = phase_by_id["implementation"].get("paths", [])
    for later_id in ("gate", "seal"):
        for owned in implementation:
            for later in phase_by_id[later_id].get("paths", []):
                if patterns_overlap(owned, later):
                    errors.append(
                        f"implementation owns {later_id} path: {owned} overlaps {later}"
                    )
    for path in phase_by_id["seal"].get("paths", []):
        if (
            path.endswith((".c", ".h", ".py", ".php", ".phpt"))
            or "/tests/" in f"/{path}"
        ):
            errors.append(f"seal contains code or test path: {path}")
    return errors


def validate_range(
    manifest: dict[str, Any],
    phase_id: str,
    base: str,
    head: str,
) -> list[str]:
    phase = next(
        (item for item in manifest["phases"] if item["phase_id"] == phase_id),
        None,
    )
    if phase is None:
        return [f"unknown phase: {phase_id}"]
    if phase.get("write_mode") == "read_only":
        return [f"{phase_id} is read-only and cannot own a commit range"]
    allowed = phase.get("paths", [])
    return [
        f"{phase_id}: changed foreign path {path}"
        for path in changed_paths(base, head)
        if not any(path_matches(path, pattern) for pattern in allowed)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--phase", choices=EXPECTED_PHASES)
    parser.add_argument("--base")
    parser.add_argument("--head")
    args = parser.parse_args()
    if not args.check and args.phase is None:
        parser.error("use --check and/or --phase")
    if (args.base is None) != (args.head is None):
        parser.error("--base and --head must be provided together")
    if args.phase is not None and args.base is None:
        parser.error("--phase requires --base and --head")

    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        errors = validate_manifest(manifest)
        if args.phase is not None:
            errors.extend(
                validate_range(manifest, args.phase, args.base, args.head)
            )
    except (OSError, json.JSONDecodeError, KeyError, PhaseError) as exc:
        print(f"W05 phase validation failed: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"W05 phase validation failed: {error}", file=sys.stderr)
        return 1
    print("W05 phase manifest valid: eight serial phases, no unplanned overlap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
