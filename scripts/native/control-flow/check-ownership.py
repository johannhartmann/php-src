#!/usr/bin/env python3
"""Validate W04 ownership and generate its wave definition."""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs/native-engine/control-flow/w04-ownership.json"
SOURCE_MANIFEST = ROOT / "docs/native-engine/control-flow/w04-source-files.json"
CONFIG_M4 = ROOT / "ext/native_mir_test/config.m4"
WAVES = ROOT / "docs/native-engine/waves/waves.json"

EXPECTED_SPECIALISTS = (
    "W04-A-production-control-flow",
    "W04-B-control-flow-evidence",
)


class OwnershipError(RuntimeError):
    """A frozen path or ownership invariant was violated."""


def _has_magic(pattern: str) -> bool:
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
        if not _has_magic(right):
            return path_matches(right, left)
        return right.startswith(prefix + "/")
    if right.endswith("/**"):
        return patterns_overlap(right, left)
    if not _has_magic(left) and not _has_magic(right):
        return left == right
    if not _has_magic(left):
        return fnmatchcase(left, right)
    if not _has_magic(right):
        return fnmatchcase(right, left)
    left_parent = left.rsplit("/", 1)[0] if "/" in left else ""
    right_parent = right.rsplit("/", 1)[0] if "/" in right else ""
    return left_parent == right_parent


def _validate_task(task: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for field in ("task_id", "role", "title", "owned_paths"):
        if field not in task:
            errors.append(f"task lacks {field}")
    paths = task.get("owned_paths", [])
    if not isinstance(paths, list) or not paths:
        errors.append(f"{task.get('task_id', '<unknown>')}: owned_paths is empty")
    if len(paths) != len(set(paths)):
        errors.append(f"{task.get('task_id', '<unknown>')}: duplicate owned path")
    return errors


def _config_sources() -> list[str]:
    text = CONFIG_M4.read_text(encoding="utf-8")
    sources: list[str] = []
    for match in re.finditer(
        r"PHP_ADD_SOURCES\(\[([^\]]+)\],\s*\[([^\]]+)\]\)",
        text,
        re.MULTILINE,
    ):
        directory = match.group(1).strip()
        for name in match.group(2).split():
            sources.append(f"{directory}/{name}")
    return sources


def validate_manifest(
    manifest: dict[str, Any],
    source_manifest: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    if manifest.get("format_version") != 1 or manifest.get("wave") != "W04":
        errors.append("ownership format or wave is not frozen W04 version 1")
    specialists = manifest.get("specialist_tasks", [])
    ids = tuple(task.get("task_id") for task in specialists)
    if ids != EXPECTED_SPECIALISTS:
        errors.append("specialist task identities/order must be exactly W04-A and W04-B")
    if len(specialists) != 2:
        errors.append("W04 must contain exactly two specialist tasks")
    for task in specialists:
        errors.extend(_validate_task(task))
    integration = manifest.get("integration_task", {})
    errors.extend(_validate_task(integration))
    if integration.get("task_id") != "W04-integration-gate":
        errors.append("integration task identity must be W04-integration-gate")

    reserved = manifest.get("contract_reserved_paths", [])
    if not isinstance(reserved, list) or not reserved:
        errors.append("contract_reserved_paths is empty")
        reserved = []

    for index, left_task in enumerate(specialists):
        for right_task in specialists[index + 1 :]:
            for left in left_task.get("owned_paths", []):
                for right in right_task.get("owned_paths", []):
                    if patterns_overlap(left, right):
                        errors.append(
                            f"specialist overlap: {left_task['task_id']}:{left} and "
                            f"{right_task['task_id']}:{right}"
                        )

    protected = [
        *reserved,
        *integration.get("owned_paths", []),
    ]
    for task in specialists:
        for owned in task.get("owned_paths", []):
            for blocked in protected:
                if patterns_overlap(owned, blocked):
                    errors.append(
                        f"{task['task_id']} owns reserved/integration path: "
                        f"{owned} overlaps {blocked}"
                    )

    if source_manifest.get("format_version") != 1:
        errors.append("source-file manifest format is not version 1")
    if source_manifest.get("consumer") != "ext/native_mir_test/config.m4":
        errors.append("source-file manifest consumer is not config.m4")
    current_sources = source_manifest.get("existing_production_sources", [])
    if current_sources != _config_sources():
        errors.append("existing production source manifest drifted from config.m4")
    w04_sources = source_manifest.get("w04_production_sources", [])
    if len(w04_sources) != len(set(w04_sources)) or not w04_sources:
        errors.append("W04 production source manifest is empty or has duplicates")
    a_paths = specialists[0].get("owned_paths", []) if specialists else []
    for source in w04_sources:
        if not any(path_matches(source, pattern) for pattern in a_paths):
            errors.append(f"W04 production source is outside A ownership: {source}")
    return errors


def changed_paths(base: str, head: str) -> list[str]:
    completed = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}", "--"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise OwnershipError(completed.stderr.strip() or "git diff failed")
    return [line for line in completed.stdout.splitlines() if line]


def validate_task_head(
    manifest: dict[str, Any],
    task_id: str,
    base: str,
    head: str,
) -> list[str]:
    errors: list[str] = []
    task = next(
        (item for item in manifest["specialist_tasks"] if item["task_id"] == task_id),
        None,
    )
    if task is None:
        return [f"unknown specialist task: {task_id}"]
    reserved = [
        *manifest["contract_reserved_paths"],
        *manifest["integration_task"]["owned_paths"],
    ]
    for path in changed_paths(base, head):
        if any(path_matches(path, pattern) for pattern in reserved):
            errors.append(f"{task_id}: changed reserved/integration path {path}")
        if not any(path_matches(path, pattern) for pattern in task["owned_paths"]):
            errors.append(f"{task_id}: changed foreign path {path}")
    return errors


def build_wave(manifest: dict[str, Any], expected_base: str) -> dict[str, Any]:
    if re.fullmatch(r"[0-9a-f]{40}", expected_base) is None:
        raise OwnershipError("expected base must be a full lowercase commit hash")
    specialists = manifest["specialist_tasks"]
    integration = manifest["integration_task"]
    tasks = []
    for task in [*specialists, integration]:
        tasks.append(
            {
                "owned_paths": task["owned_paths"],
                "requires_clean_worktree": True,
                "role": task["role"],
                "task_id": task["task_id"],
                "title": task["title"],
            }
        )
    responsible_paths: list[str] = []
    for task in [*specialists, integration]:
        for path in task["owned_paths"]:
            if path not in responsible_paths:
                responsible_paths.append(path)
    return {
        "dependencies": ["W03"],
        "expected_base_commit": expected_base,
        "goal": (
            "Lower source-backed reducible Zend control flow, preserve exact "
            "successor/PHI order, and verify stage-3 block, edge, loop, and "
            "edge-statepoint mappings."
        ),
        "optional_gate_ids": [],
        "parallel_tracks": [
            "A: production source-backed control flow",
            "B: control-flow evidence"
        ],
        "required_gate_ids": [
            specialists[0]["task_id"],
            specialists[1]["task_id"],
            integration["task_id"],
        ],
        "responsible_paths": responsible_paths,
        "roles": [
            specialists[0]["role"],
            specialists[1]["role"],
            integration["role"],
        ],
        "tasks": tasks,
        "title": "Control flow and loops",
        "wave_id": "W04",
    }


def write_wave_definition(manifest: dict[str, Any], expected_base: str) -> None:
    document = json.loads(WAVES.read_text(encoding="utf-8"))
    replacement = build_wave(manifest, expected_base)
    matches = [
        index
        for index, wave in enumerate(document["waves"])
        if wave.get("wave_id") == "W04"
    ]
    if len(matches) > 1:
        raise OwnershipError("waves.json contains duplicate W04 definitions")
    if matches:
        document["waves"][matches[0]] = replacement
    else:
        document["waves"].append(replacement)
    WAVES.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-manifest", action="store_true")
    parser.add_argument("--task", choices=EXPECTED_SPECIALISTS)
    parser.add_argument("--base")
    parser.add_argument("--head")
    parser.add_argument("--write-wave-definition", metavar="H")
    args = parser.parse_args()
    if not any((args.check_manifest, args.task, args.write_wave_definition)):
        parser.error("select --check-manifest, --task, or --write-wave-definition")
    if args.task and (not args.base or not args.head):
        parser.error("--task requires --base and --head")

    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        source_manifest = json.loads(SOURCE_MANIFEST.read_text(encoding="utf-8"))
        errors = validate_manifest(manifest, source_manifest)
        if args.task and not errors:
            errors.extend(
                validate_task_head(manifest, args.task, args.base, args.head)
            )
        if errors:
            raise OwnershipError("; ".join(errors))
        if args.write_wave_definition:
            write_wave_definition(manifest, args.write_wave_definition)
    except (OSError, KeyError, TypeError, json.JSONDecodeError, OwnershipError) as error:
        print(f"W04 ownership check failed: {error}", file=sys.stderr)
        return 1

    if args.write_wave_definition:
        print(f"W04 wave definition generated with base {args.write_wave_definition}")
    elif args.task:
        print(f"W04 ownership check passed for {args.task}: {args.base}..{args.head}")
    else:
        print("W04 ownership manifest passed: two disjoint specialist path sets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
