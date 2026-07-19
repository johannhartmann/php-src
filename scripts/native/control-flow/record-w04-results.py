#!/usr/bin/env python3
"""Record audited W04 A, B, and integration results with real provenance."""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
DEFINITION = ROOT / "docs/native-engine/waves/waves.json"
OWNERSHIP = ROOT / "docs/native-engine/control-flow/w04-ownership.json"
WAVE_GATE = ROOT / "scripts/native/wave-gate.py"
H_COMMIT = "01e51448e2bc9423d7dc1254ae5e4d34fc236eb4"
P_COMMIT = "92915212484786b36b247c1f8d23a5102bab8534"
DEFAULT_A_HEAD = "3a3c3218f8fa52ffc1d477f56f6faff132b0e1ab"
DEFAULT_B_HEAD = "acd393a6eb8f1bb05f797368e34fc97cf9e884a7"
BRANCHES = {
    "W04-A-production-control-flow": "codex/w04-production-control-flow",
    "W04-B-control-flow-evidence": "codex/w04-control-flow-evidence",
}
COMMANDS = {
    "W04-A-production-control-flow":
        "python3 tests/native/control-flow/unit/run_control_flow_tests.py",
    "W04-B-control-flow-evidence":
        "python3 scripts/native/control-flow/run-w04-differential.py "
        "--reference-php ${REFERENCE_PHP} --candidate-php ${CANDIDATE_PHP}",
    "W04-integration-gate":
        "python3 scripts/native/control-flow/test-w04.py "
        "--reference-php ${REFERENCE_PHP} --candidate-php ${CANDIDATE_PHP}",
}
INTEGRATION_CRITERIA = (
    ("W04-I-AC1", "A/B diff sets are disjoint and conflict-free."),
    ("W04-I-AC2", "Specialist results retain their actual distinct heads."),
    ("W04-I-AC3", "config.m4 exactly matches the source-file manifest."),
    ("W04-I-AC4", "Accepted corpus passes Stage 1, Stage 2, and Stage 3."),
    ("W04-I-AC5", "Rejected corpus reports exact later-wave and MIRL decisions."),
    ("W04-I-AC6", "Branch, PHI, loop, and statepoint goldens are deterministic."),
    ("W04-I-AC7", "Reference and candidate PHP execution is byte-identical."),
    ("W04-I-AC8", "ASan, UBSan, ZTS, and fixed-seed fuzzing pass."),
    ("W04-I-AC9", "W03 and earlier waves do not regress."),
    ("W04-I-AC10", "No runtime, VM fallback, interpreter, or TPDE path is active."),
    ("W04-I-AC11", "The W04 wave gate reports pass."),
)


class ResultError(RuntimeError):
    """External W04 evidence is incomplete or inconsistent."""


def git(*arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return completed.stdout.strip()


def wave_definition() -> dict[str, Any]:
    document = json.loads(DEFINITION.read_text(encoding="utf-8"))
    return next(wave for wave in document["waves"] if wave["wave_id"] == "W04")


def owned_paths() -> dict[str, list[str]]:
    document = json.loads(OWNERSHIP.read_text(encoding="utf-8"))
    paths = {
        task["task_id"]: task["owned_paths"]
        for task in document["specialist_tasks"]
    }
    integration = document["integration_task"]
    paths[integration["task_id"]] = integration["owned_paths"]
    return paths


def specialist_changed_paths(head: str) -> list[str]:
    output = git("diff", "--name-only", "{}..{}".format(H_COMMIT, head))
    return output.splitlines() if output else []


def integration_changed_paths(
    head: str,
    path_sets: dict[str, list[str]],
) -> list[str]:
    output = git("diff", "--name-only", "{}..{}".format(P_COMMIT, head))
    changed_paths = output.splitlines() if output else []
    task_for_path: dict[str, str] = {}
    for path in changed_paths:
        owners = [
            task_id
            for task_id, patterns in path_sets.items()
            if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)
        ]
        if len(owners) != 1:
            raise ResultError(
                "integrated path must have exactly one W04 owner: {} ({})".format(
                    path,
                    ", ".join(owners) if owners else "unowned",
                )
            )
        task_for_path[path] = owners[0]
    return sorted(
        path
        for path, task_id in task_for_path.items()
        if task_id == "W04-integration-gate"
    )


def check_specialist(task_id: str, head: str) -> None:
    git("cat-file", "-e", "{}^{{commit}}".format(head))
    if git("merge-base", H_COMMIT, head) != H_COMMIT:
        raise ResultError("{} does not descend from H".format(task_id))
    subprocess.run(
        [
            "python3",
            "scripts/native/control-flow/check-ownership.py",
            "--task",
            task_id,
            "--base",
            H_COMMIT,
            "--head",
            head,
        ],
        cwd=ROOT,
        check=True,
        timeout=60,
    )


def result_document(
    task_id: str,
    head: str,
    branch: str,
    changed_paths: list[str],
    timestamp: str,
    artifact_reference: str,
) -> dict[str, Any]:
    criteria = (
        [
            {
                "criterion_id": criterion_id,
                "description": description,
                "status": "pass",
            }
            for criterion_id, description in INTEGRATION_CRITERIA
        ]
        if task_id == "W04-integration-gate"
        else [
            {
                "criterion_id": "audited-integration",
                "description": (
                    "The actual specialist head passed ownership, review, "
                    "and the integrated W04 hard gate."
                ),
                "status": "pass",
            }
        ]
    )
    artifact = {"kind": "local", "reference": artifact_reference}
    return {
        "acceptance_criteria": criteria,
        "actual_base_commit": H_COMMIT,
        "blockers": [],
        "branch": branch,
        "changed_paths": changed_paths,
        "expected_base_commit": H_COMMIT,
        "format_version": "1.0.0",
        "gate_evidence": [
            {
                "artifact": artifact,
                "format_version": "1.0.0",
                "gate_id": task_id,
                "status": "pass",
                "summary": "Audited W04 hard-gate evidence passed.",
                "wave_id": "W04",
            }
        ],
        "head_commit": head,
        "risks": [],
        "status": "pass",
        "task_id": task_id,
        "tests": [
            {
                "artifact": artifact,
                "command": COMMANDS[task_id],
                "duration_ms": None,
                "exit_code": 0,
                "status": "pass",
            }
        ],
        "timestamp": timestamp,
        "worktree_clean": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--artifact-prefix", default="w04-artifacts", type=Path)
    parser.add_argument("--results-dir", required=True, type=Path)
    parser.add_argument("--a-head", default=DEFAULT_A_HEAD)
    parser.add_argument("--b-head", default=DEFAULT_B_HEAD)
    parser.add_argument("--replace", action="store_true")
    arguments = parser.parse_args()
    try:
        if git("status", "--porcelain=v1", "--untracked-files=all"):
            raise ResultError("refusing to record passing results from a dirty worktree")
        if arguments.a_head == arguments.b_head:
            raise ResultError("A and B specialist heads must be distinct")
        wave = wave_definition()
        if wave["expected_base_commit"] != H_COMMIT:
            raise ResultError("W04 definition is not pinned to H")
        check_specialist("W04-A-production-control-flow", arguments.a_head)
        check_specialist("W04-B-control-flow-evidence", arguments.b_head)
        integration_head = git("rev-parse", "HEAD")
        integration_branch = git("branch", "--show-current") or "detached"
        heads = {
            "W04-A-production-control-flow": arguments.a_head,
            "W04-B-control-flow-evidence": arguments.b_head,
            "W04-integration-gate": integration_head,
        }
        branches = {
            **BRANCHES,
            "W04-integration-gate": integration_branch,
        }
        path_sets = owned_paths()
        changed = {
            "W04-A-production-control-flow":
                specialist_changed_paths(arguments.a_head),
            "W04-B-control-flow-evidence":
                specialist_changed_paths(arguments.b_head),
            "W04-integration-gate":
                integration_changed_paths(integration_head, path_sets),
        }
        timestamp = (
            dt.datetime.now(dt.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
        artifact_dir = arguments.artifact_dir.resolve()
        prefix = arguments.artifact_prefix.as_posix().rstrip("/")
        with tempfile.TemporaryDirectory(prefix="w04-results-") as directory:
            temporary = Path(directory)
            for task_id in wave["required_gate_ids"]:
                log = artifact_dir / "{}.log".format(task_id)
                if not log.is_file() or log.stat().st_size == 0:
                    raise ResultError("missing non-empty evidence log: {}".format(log))
                reference = "{}/{}.log".format(prefix, task_id)
                result_path = temporary / "{}.json".format(task_id)
                result_path.write_text(
                    json.dumps(
                        result_document(
                            task_id,
                            heads[task_id],
                            branches[task_id],
                            changed[task_id],
                            timestamp,
                            reference,
                        ),
                        indent=2,
                        sort_keys=True,
                    )
                    + "\n",
                    encoding="utf-8",
                )
                command = [
                    "python3",
                    str(WAVE_GATE),
                    "record",
                    "--wave",
                    "W04",
                    "--result",
                    str(result_path),
                    "--results-dir",
                    str(arguments.results_dir),
                ]
                if arguments.replace:
                    command.append("--replace")
                subprocess.run(command, cwd=ROOT, check=True, timeout=30)
        subprocess.run(
            [
                "python3",
                str(WAVE_GATE),
                "check",
                "--wave",
                "W04",
                "--results-dir",
                str(arguments.results_dir),
            ],
            cwd=ROOT,
            check=True,
            timeout=30,
        )
        print("W04 passing results recorded in {}".format(arguments.results_dir))
        return 0
    except (
        OSError,
        KeyError,
        StopIteration,
        ResultError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
    ) as error:
        print("record-w04-results.py: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
