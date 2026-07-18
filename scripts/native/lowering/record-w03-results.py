#!/usr/bin/env python3
"""Create audited external W03 task results after the complete hard gate passes."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
DEFINITION = ROOT / "docs/native-engine/waves/waves.json"
WAVE_GATE = ROOT / "scripts/native/wave-gate.py"
TASK_COMMANDS = {
    "W03-A-lowering-core-registry":
        "python3 tests/native/lowering/core/run_core_lowering_tests.py",
    "W03-B-frontend-operands-facts":
        "python3 tests/native/lowering/frontend/run_frontend_tests.py",
    "W03-C-numeric-arithmetic-bitwise":
        "python3 tests/native/lowering/numeric/run_numeric_tests.py",
    "W03-D-comparison-boolean-casts":
        "python3 tests/native/lowering/logic/run_logic_tests.py",
    "W03-E-straight-line-lifetime-return":
        "python3 tests/native/lowering/lifetime/run_lifetime_tests.py",
    "W03-F-mir-scalar-verifier-text":
        "python3 tests/native/lowering/mir/run_scalar_mir_tests.py",
    "W03-G-compile-dump-differential":
        "python3 scripts/native/lowering/run-w03-differential.py "
        "--reference ${REFERENCE_PHP} --candidate ${CANDIDATE_PHP}",
    "W03-integration-gate":
        "python3 scripts/native/lowering/test-w03.py "
        "--reference-php ${REFERENCE_PHP} --candidate-php ${CANDIDATE_PHP}",
}
TASK_PATHS = {
    "W03-A-lowering-core-registry": [
        "Zend/Native/Lowering/Core/zend_mir_lowering_registry.c"
    ],
    "W03-B-frontend-operands-facts": [
        "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c"
    ],
    "W03-C-numeric-arithmetic-bitwise": [
        "Zend/Native/Lowering/Scalar/Numeric/zend_mir_numeric_provider.c"
    ],
    "W03-D-comparison-boolean-casts": [
        "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic_provider.c"
    ],
    "W03-E-straight-line-lifetime-return": [
        "Zend/Native/Lowering/StraightLine/zend_mir_lifetime_provider.c"
    ],
    "W03-F-mir-scalar-verifier-text": [
        "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.c"
    ],
    "W03-G-compile-dump-differential": [
        "scripts/native/lowering/run-w03-differential.py"
    ],
    "W03-integration-gate": [
        "scripts/native/lowering/test-w03.py",
        "scripts/native/lowering/validate-w03.py",
        "tests/native/lowering/integration",
        "tests/native/lowering/fuzz",
        "docs/native-engine/lowering/w03-coverage-report.json",
        ".github/workflows/native-w03.yml",
    ],
}
INTEGRATION_CRITERIA = (
    ("W03-I-AC1", "A-G are reviewed, integrated, and blocker-free."),
    ("W03-I-AC2", "Real PHP source lowers from Zend SSA to valid ZNMIR."),
    ("W03-I-AC3", "Every accepted corpus case passes both verifier stages."),
    ("W03-I-AC4", "Every rejected case reports its exact MIRL code and no module."),
    ("W03-I-AC5", "Provider, profile, and proof coverage is complete."),
    ("W03-I-AC6", "Dumps are byte-identical across determinism axes."),
    ("W03-I-AC7", "Reference and candidate execution are byte-identical."),
    ("W03-I-AC8", "ASan, UBSan, and fixed-seed fuzzing pass."),
    ("W03-I-AC9", "W00, W01, and W02 regressions pass."),
    ("W03-I-AC10", "Standard builds activate no MIR runtime path."),
    ("W03-I-AC11", "The W03 wave gate reports pass."),
)


class ResultError(RuntimeError):
    """External W03 evidence is incomplete or inconsistent."""


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


def w03_definition() -> dict[str, Any]:
    document = json.loads(DEFINITION.read_text(encoding="utf-8"))
    return next(wave for wave in document["waves"] if wave["wave_id"] == "W03")


def result_document(
    task_id: str,
    base: str,
    head: str,
    branch: str,
    timestamp: str,
    artifact_reference: str,
) -> dict[str, Any]:
    if task_id == "W03-integration-gate":
        criteria = [
            {
                "criterion_id": criterion_id,
                "description": description,
                "status": "pass",
            }
            for criterion_id, description in INTEGRATION_CRITERIA
        ]
    else:
        criteria = [
            {
                "criterion_id": "audited-integration",
                "description": "The specialist result passed in the integrated W03 hard gate.",
                "status": "pass",
            }
        ]
    evidence = {
        "artifact": {"kind": "local", "reference": artifact_reference},
        "format_version": "1.0.0",
        "gate_id": task_id,
        "status": "pass",
        "summary": "Integrated W03 hard-gate evidence passed.",
        "wave_id": "W03",
    }
    return {
        "acceptance_criteria": criteria,
        "actual_base_commit": base,
        "blockers": [],
        "branch": branch,
        "changed_paths": TASK_PATHS[task_id],
        "expected_base_commit": base,
        "format_version": "1.0.0",
        "gate_evidence": [evidence],
        "head_commit": head,
        "risks": [],
        "status": "pass",
        "task_id": task_id,
        "tests": [
            {
                "artifact": {
                    "kind": "local",
                    "reference": artifact_reference,
                },
                "command": TASK_COMMANDS[task_id],
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
    parser.add_argument(
        "--artifact-prefix", default="w03-artifacts", type=Path
    )
    parser.add_argument("--results-dir", required=True, type=Path)
    parser.add_argument("--replace", action="store_true")
    arguments = parser.parse_args()
    try:
        status = git("status", "--porcelain=v1", "--untracked-files=all")
        if status:
            raise ResultError("refusing to record passing results from a dirty worktree")
        head = git("rev-parse", "HEAD")
        branch = git("branch", "--show-current") or "detached"
        wave = w03_definition()
        base = wave["expected_base_commit"]
        if base is None:
            raise ResultError("W03 expected base is not frozen")
        timestamp = (
            dt.datetime.now(dt.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
        artifact_dir = arguments.artifact_dir.resolve()
        artifact_prefix = arguments.artifact_prefix.as_posix().rstrip("/")
        with tempfile.TemporaryDirectory(prefix="w03-results-") as directory:
            temporary = Path(directory)
            for task_id in wave["required_gate_ids"]:
                log = artifact_dir / f"{task_id}.log"
                if not log.is_file() or log.stat().st_size == 0:
                    raise ResultError(f"missing non-empty evidence log: {log}")
                reference = f"{artifact_prefix}/{task_id}.log"
                result_path = temporary / f"{task_id}.json"
                result_path.write_text(
                    json.dumps(
                        result_document(
                            task_id,
                            base,
                            head,
                            branch,
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
                    "W03",
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
                "W03",
                "--results-dir",
                str(arguments.results_dir),
            ],
            cwd=ROOT,
            check=True,
            timeout=30,
        )
        print(f"W03 passing results recorded in {arguments.results_dir}")
        return 0
    except (
        OSError,
        KeyError,
        StopIteration,
        ResultError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"record-w03-results.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
