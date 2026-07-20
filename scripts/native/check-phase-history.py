#!/usr/bin/env python3
"""Check every W05-v2 commit trailer and changed path."""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "docs/native-engine/calls/w05-phase-manifest.json"


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def matches(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        root = pattern[:-3].rstrip("/")
        return path == root or path.startswith(root + "/")
    return fnmatchcase(path, pattern)


def parent_count(commit: str) -> int:
    """Return the number of parents without relying on diff-tree merge modes."""
    parents = git("rev-list", "--parents", "-n", "1", commit).split()
    return max(0, len(parents) - 1)


def validate() -> list[str]:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    start = data["start_commit"]
    phases = {phase["phase_id"]: phase for phase in data["phases"]}
    ordered = [phase["phase_id"] for phase in data["phases"]]
    commits = git("rev-list", "--reverse", f"{start}..HEAD").splitlines()
    errors: list[str] = []
    last_order = -1
    for commit in commits:
        if parent_count(commit) != 1:
            errors.append(f"{commit}: wave history must not contain merge commits")
            continue
        body = git("show", "-s", "--format=%B", commit)
        trailers = [
            line.removeprefix("Native-Phase:").strip()
            for line in body.splitlines()
            if line.startswith("Native-Phase:")
        ]
        if len(trailers) != 1:
            errors.append(f"{commit}: expected exactly one Native-Phase trailer")
            continue
        phase_id = trailers[0]
        phase = phases.get(phase_id)
        if phase is None:
            errors.append(f"{commit}: unknown phase {phase_id}")
            continue
        order = ordered.index(phase_id)
        if order < last_order:
            errors.append(f"{commit}: phase {phase_id} is out of order")
        if order == last_order and not phase.get("repeatable", False):
            errors.append(f"{commit}: phase {phase_id} is not repeatable")
        if order > last_order + 1:
            errors.append(f"{commit}: skipped required phase before {phase_id}")
        last_order = max(last_order, order)
        paths = git("diff-tree", "--no-commit-id", "--name-only", "-r", commit).splitlines()
        for path in paths:
            if not any(matches(path, pattern) for pattern in phase["paths"]):
                errors.append(f"{commit}: {phase_id} changed foreign path {path}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wave", required=True, choices=["W05-v2"])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required")
    try:
        errors = validate()
    except (OSError, json.JSONDecodeError, RuntimeError) as exc:
        errors = [str(exc)]
    if errors:
        for error in errors:
            print(f"W05-v2 history invalid: {error}", file=sys.stderr)
        return 1
    print("W05-v2 history valid: commit-level ownership and trailers pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
