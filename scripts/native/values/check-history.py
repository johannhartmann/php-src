#!/usr/bin/env python3
"""Validate W06 phase ownership and commit trailers."""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs/native-engine/values/w06-phase-manifest.json"
EXPECTED = (
    "W06-contract-H",
    "W06-wave-P",
    "W06-core-A1",
    "W06-lowering-A2",
    "W06-gate-I",
    "W06-seal-S",
)


def matches(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        prefix = pattern[:-3].rstrip("/")
        return path == prefix or path.startswith(prefix + "/")
    return fnmatchcase(path, pattern)


def overlaps(left: str, right: str) -> bool:
    if left == right:
        return True
    if left.endswith("/**"):
        prefix = left[:-3].rstrip("/")
        return right.startswith(prefix + "/") or fnmatchcase(right, left)
    if right.endswith("/**"):
        prefix = right[:-3].rstrip("/")
        return left.startswith(prefix + "/") or fnmatchcase(left, right)
    return fnmatchcase(left, right) or fnmatchcase(right, left)


def git(*args: str) -> str:
    completed = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True, check=False
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    return completed.stdout


def validate_manifest(data: dict[str, object]) -> list[str]:
    errors: list[str] = []
    if data.get("format_version") != 1 or data.get("wave") != "W06":
        errors.append("manifest identity drift")
    if data.get("writer_branch") != "integration/wave-06":
        errors.append("writer branch drift")
    phases = data.get("phases")
    if not isinstance(phases, list):
        return errors + ["phases must be an array"]
    ids = tuple(phase.get("phase_id") for phase in phases if isinstance(phase, dict))
    if ids != EXPECTED:
        return errors + [f"phase order drift: {ids}"]
    for index, phase in enumerate(phases):
        if phase.get("order") != index:
            errors.append(f"{phase.get('phase_id')}: order drift")
        if not isinstance(phase.get("paths"), list) or not phase["paths"]:
            errors.append(f"{phase.get('phase_id')}: empty paths")
    for index, left_phase in enumerate(phases):
        for right_phase in phases[index + 1:]:
            for left in left_phase["paths"]:
                for right in right_phase["paths"]:
                    if overlaps(left, right):
                        errors.append(
                            f"phase overlap: {left_phase['phase_id']}:{left} "
                            f"and {right_phase['phase_id']}:{right}"
                        )
    aliases = data.get("repair_phase_aliases")
    if not isinstance(aliases, dict):
        errors.append("repair aliases missing")
    elif any(target not in EXPECTED for target in aliases.values()):
        errors.append("repair alias target drift")
    return errors


def commit_phase(commit: str) -> str:
    body = git("show", "-s", "--format=%B", commit)
    trailers = [
        line.split(":", 1)[1].strip()
        for line in body.splitlines()
        if line.startswith("Native-Phase:")
    ]
    if len(trailers) != 1:
        raise RuntimeError(f"{commit}: expected exactly one Native-Phase trailer")
    return trailers[0]


def changed_paths(base: str, head: str) -> list[str]:
    return [
        line for line in git("diff", "--name-only", f"{base}..{head}").splitlines()
        if line
    ]


def validate_range(
    data: dict[str, object], phase: str, base: str, head: str
) -> list[str]:
    phases = {item["phase_id"]: item for item in data["phases"]}
    aliases = data["repair_phase_aliases"]
    owner = aliases.get(phase, phase)
    if owner not in phases:
        return [f"unknown phase: {phase}"]
    errors: list[str] = []
    if git("merge-base", base, head).strip() != base:
        errors.append(f"{base} is not an ancestor of {head}")
    commits = git("rev-list", "--reverse", f"{base}..{head}").splitlines()
    if not commits:
        errors.append("phase range contains no commit")
    for commit in commits:
        try:
            actual = commit_phase(commit)
        except RuntimeError as exc:
            errors.append(str(exc))
            continue
        if actual != phase:
            errors.append(f"{commit}: trailer {actual}, expected {phase}")
    allowed = phases[owner]["paths"]
    foreign = [
        path for path in changed_paths(base, head)
        if not any(matches(path, pattern) for pattern in allowed)
    ]
    if foreign:
        errors.append(f"{phase}: foreign paths: {foreign}")
    return errors


def validate_current_history(data: dict[str, object]) -> list[str]:
    start = data["start_commit"]
    head = git("rev-parse", "HEAD").strip()
    if git("merge-base", start, head).strip() != start:
        return [f"start commit {start} is not an ancestor of HEAD"]
    phases = {item["phase_id"]: item for item in data["phases"]}
    aliases = data["repair_phase_aliases"]
    last_order = -1
    errors: list[str] = []
    for commit in git("rev-list", "--reverse", f"{start}..{head}").splitlines():
        try:
            phase = commit_phase(commit)
        except RuntimeError as exc:
            errors.append(str(exc))
            continue
        owner = aliases.get(phase, phase)
        if owner not in phases:
            errors.append(f"{commit}: phase {phase} absent from manifest")
            continue
        order = phases[owner]["order"]
        if phase not in aliases and order < last_order:
            errors.append(f"{commit}: non-linear phase order")
        last_order = max(last_order, order)
        parent = f"{commit}^"
        allowed = phases[owner]["paths"]
        foreign = [
            path for path in changed_paths(parent, commit)
            if not any(matches(path, pattern) for pattern in allowed)
        ]
        if foreign:
            errors.append(f"{commit}: foreign paths for {phase}: {foreign}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--phase")
    parser.add_argument("--base")
    parser.add_argument("--head")
    args = parser.parse_args()
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        errors = validate_manifest(data)
        if args.phase or args.base or args.head:
            if not (args.phase and args.base and args.head):
                parser.error("--phase, --base and --head must be supplied together")
            errors.extend(validate_range(data, args.phase, args.base, args.head))
        elif args.check:
            errors.extend(validate_current_history(data))
        else:
            parser.error("--check or a complete phase range is required")
    except (OSError, json.JSONDecodeError, KeyError, RuntimeError) as exc:
        errors = [str(exc)]
    if errors:
        for error in errors:
            print(f"W06 history invalid: {error}", file=sys.stderr)
        return 1
    print("W06 history valid: disjoint linear phase ownership")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
